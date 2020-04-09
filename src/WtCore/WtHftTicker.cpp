/*!
 * \file WtHftTicker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtHftTicker.h"
#include "WtHftEngine.h"
#include "IDataReader.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/WTSSessionInfo.hpp"
#include "../Share/IBaseDataMgr.h"
#include "../Share/WTSDataDef.hpp"
#include "../Share/IHotMgr.h"
#include "../Share/CodeHelper.hpp"

#include "../WTSTools/WTSLogger.h"

USING_NS_OTP;


WtHftRtTicker::WtHftRtTicker(WtHftEngine* engine)
	: _engine(engine)
	, _stopped(false)
	, _date(0)
	, _time(UINT_MAX)
	, _next_check_time(0)
	, _last_emit_pos(0)
	, _cur_pos(0)
{
}


WtHftRtTicker::~WtHftRtTicker()
{
}

void WtHftRtTicker::init(IDataReader* store, const char* sessionID)
{
	_store = store;
	_s_info = _engine->get_session_info(sessionID);

	TimeUtils::getDateTime(_date, _time);
}

void WtHftRtTicker::trigger_price(WTSTickData* curTick, bool isHot/* = false*/)
{
	if (_engine)
	{
		std::string stdCode = curTick->code();
		_engine->on_tick(stdCode.c_str(), curTick);

		if (isHot)
		{
			WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
			std::string hotCode = CodeHelper::stdCodeToStdHotCode(stdCode.c_str());
			strcpy(hotTick->getTickStruct().code, hotCode.c_str());
			_engine->on_tick(hotCode.c_str(), hotTick);
			hotTick->release();
		}
	}
}

void WtHftRtTicker::on_tick(WTSTickData* curTick, bool isHot /* = false */)
{
	if (_thrd == NULL)
	{
		trigger_price(curTick, isHot);
		return;
	}

	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("����ʱ��%uС�ڱ���ʱ��%u", uTime, _time);
		trigger_price(curTick, isHot);
		return;
	}

	_date = uDate;
	_time = uTime;

	uint32_t curMin = _time / 100000;
	uint32_t curSec = _time % 100000;
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	bool isSecEnd = _s_info->isLastOfSection(curMin);
	if (isSecEnd)
	{
		minutes--;
	}
	minutes++;
	curMin = _s_info->minuteToTime(minutes);

	if (_cur_pos == 0)
	{
		//�����ǰʱ����0, ��ֱ�Ӹ�ֵ����
		_cur_pos = minutes;
	}
	else if (_cur_pos < minutes)
	{
		//����Ѽ�¼�ķ���С���µķ���, ����Ҫ�����պ��¼�
		//���ʱ��Ҫ�ȴ����պ�, ���޸�ƽ̨ʱ��ͼ۸�
		if (_last_emit_pos < _cur_pos)
		{
			//�������ݻط�ģ��
			BoostUniqueLock lock(_mtx);

			//�����޸�ʱ����
			_last_emit_pos = _cur_pos;

			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);

			WTSLogger::info("������ %u.%04u �����պ�", _date, thisMin);
			if (_store)
				_store->onMinuteEnd(_date, thisMin);

			_engine->on_minute_end(_date, thisMin);

			uint32_t offMin = _s_info->offsetTime(thisMin);
			if (offMin == _s_info->getCloseTime(true))
			{
				_engine->on_session_end();
			}
		}

		trigger_price(curTick, isHot);
		if (_engine)
		{
			_engine->set_date_time(_date, curMin, curSec);
			_engine->set_trading_date(curTick->tradingdate());
		}

		_cur_pos = minutes;
	}
	else
	{
		//�������������һ�µ�, ��ֱ�Ӵ��������ʱ�伴��
		trigger_price(curTick, isHot);
		if (_engine)
			_engine->set_date_time(_date, curMin, curSec);
	}

	uint32_t sec = curSec / 1000;
	uint32_t msec = curSec % 1000;
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

void WtHftRtTicker::run()
{
	if (_thrd)
		return;

	_engine->on_init();

	uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
	_engine->set_trading_date(curTDate);

	_engine->on_session_begin();

	//�ȼ�鵱ǰʱ��, �������
	uint32_t offTime = _s_info->offsetTime(_engine->get_min_time());

	_thrd.reset(new BoostThread([this, offTime](){
		while (!_stopped)
		{
			if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				uint64_t now = TimeUtils::getLocalTimeNow();

				if (now >= _next_check_time && _last_emit_pos < _cur_pos)
				{
					//�������ݻط�ģ��
					BoostUniqueLock lock(_mtx);

					//�����޸�ʱ����
					_last_emit_pos = _cur_pos;

					uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
					_time = thisMin;

					//���thisMin��0, ˵��������
					//�����Ǳ��ؼ�ʱ���µĻ���, ˵��������ʵ����������, Ҫ�Զ�+1
					//ͬʱ��Ϊʱ����235959xxx, ����ҲҪ�ֶ���Ϊ0
					if (thisMin == 0)
					{
						uint32_t lastDate = _date;
						_date = TimeUtils::getNextDate(_date);
						_time = 0;
						WTSLogger::info("0�������Զ��л�: %u -> %u", lastDate, _date);
					}

					WTSLogger::info("������ %u.%04u �Զ��պ�", _date, thisMin);
					if (_store)
						_store->onMinuteEnd(_date, thisMin);

					_engine->on_minute_end(_date, thisMin);

					uint32_t offMin = _s_info->offsetTime(thisMin);
					if (offMin >= _s_info->getCloseTime(true))
					{
						_engine->on_session_end();
					}

					//145959000
					if (_engine)
						_engine->set_date_time(_date, thisMin, 0);
				}
			}
			else if (offTime >= _s_info->getOpenTime(true) && offTime <= _s_info->getCloseTime(true))
			{
				std::this_thread::sleep_for(std::chrono::seconds(10));
			}
		}
	}));
}

void WtHftRtTicker::stop()
{
	_stopped = true;
	if (_thrd)
		_thrd->join();
}