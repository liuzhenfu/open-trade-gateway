/////////////////////////////////////////////////////////////////////////
///@file tradectp.cpp
///@brief	CTP交易逻辑实现
///@copyright	上海信易信息科技股份有限公司 版权所有
/////////////////////////////////////////////////////////////////////////

#include "tradectp.h"
#include "utility.h"
#include "config.h"
#include "ins_list.h"
#include "numset.h"
#include "SerializerTradeBase.h"
#include "condition_order_serializer.h"

#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

traderctp::traderctp(boost::asio::io_context& ios
	, const std::string& key)
	:m_b_login(false)
	, _key(key)
	, m_settlement_info("")
	, _ios(ios)
	, _out_mq_ptr()
	, _out_mq_name(_key + "_msg_out")
	, _in_mq_ptr()
	, _in_mq_name(_key + "_msg_in")
	, _thread_ptr()
	, m_notify_seq(0)
	, m_data_seq(0)
	, _req_login()
	, m_broker_id("")
	, m_pTdApi(NULL)
	, m_trading_day("")
	, m_front_id(0)
	, m_session_id(0)
	, m_order_ref(0)
	, m_input_order_key_map()
	, m_action_order_map()
	, m_req_transfer_list()
	, _logIn_status(0)
	, _logInmutex()
	, _logInCondition()
	, m_loging_connectId(-1)
	, m_logined_connIds()
	, m_user_file_path("")
	, m_ordermap_local_remote()
	, m_ordermap_remote_local()
	, m_data()
	, m_Algorithm_Type(THOST_FTDC_AG_None)
	, m_banks()
	, m_try_req_authenticate_times(0)
	, m_try_req_login_times(0)
	, m_run_receive_msg(false)
	, m_rtn_order_log_map()
	, m_rtn_trade_log_map()
	, m_err_rtn_future_to_bank_by_future_log_map()
	, m_err_rtn_bank_to_future_by_future_log_map()
	, m_rtn_from_bank_to_future_by_future_log_map()
	, m_rtn_from_future_to_bank_by_future_log_map()
	, m_err_rtn_order_insert_log_map()
	, m_err_rtn_order_action_log_map()
	, m_condition_order_manager(_key,*this)
	, m_condition_order_task()
{
	_requestID.store(0);

	m_req_login_dt = 0;
	m_next_qry_dt = 0;
	m_next_send_dt = 0;

	m_need_query_settlement.store(false);
	m_confirm_settlement_status.store(0);
	m_req_account_id.store(0);

	m_req_position_id.store(0);
	m_rsp_position_id.store(0);

	m_rsp_account_id.store(0);
	m_need_query_bank.store(false);
	m_need_query_register.store(false);
	m_position_ready.store(false);
	m_position_inited.store(false);

	m_something_changed = false;
	m_peeking_message = false;

	m_need_save_file.store(false);

	m_need_query_broker_trading_params.store(false);

	m_is_qry_his_settlement_info.store(false);
	m_his_settlement_info = "";
	m_qry_his_settlement_info_trading_days.clear();
}

#pragma region spicallback

void traderctp::ProcessOnFrontConnected()
{
	OutputNotifyAllSycn(0, u8"已经重新连接到交易前置");
	int ret = ReqAuthenticate();
	if (0 != ret)
	{
		Log(LOG_WARNING, nullptr
			, "fun=ProcessOnFrontConnected;msg=ctp ReqAuthenticate;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, ret);
	}
}

void traderctp::OnFrontConnected()
{
	Log(LOG_INFO,nullptr
		, "fun=OnFrontConnected;key=%s;bid=%s;user_name=%s"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str());
	//还在等待登录阶段
	if (!m_b_login.load())
	{
		//这时是安全的
		OutputNotifySycn(m_loging_connectId, 0, u8"已经连接到交易前置");
		int ret = ReqAuthenticate();
		if (0 != ret)
		{
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn_status = 0;
			_logInCondition.notify_all();
		}		
	}
	else
	{
		//这时不能直接调用
		_ios.post(boost::bind(&traderctp::ProcessOnFrontConnected
			, this));
	}
}

void traderctp::ProcessOnFrontDisconnected(int nReason)
{	
	OutputNotifyAllSycn(1, u8"已经断开与交易前置的连接");
}

void traderctp::OnFrontDisconnected(int nReason)
{
	Log(LOG_WARNING, nullptr
		, "fun=OnFrontDisconnected;key=%s;bid=%s;user_name=%s;nReason=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, nReason);
	//还在等待登录阶段
	if (!m_b_login.load())
	{		
		OutputNotifySycn(m_loging_connectId, 1, u8"已经断开与交易前置的连接");
	}
	else
	{
		//这时不能直接调用
		_ios.post(boost::bind(&traderctp::ProcessOnFrontDisconnected
			, this, nReason));
	}
}

void traderctp::ProcessOnRspAuthenticate(std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if ((nullptr != pRspInfo) && (pRspInfo->ErrorID != 0))
	{
		//如果是未初始化
		if (7 == pRspInfo->ErrorID)
		{
			_ios.post(boost::bind(&traderctp::ReinitCtp, this));
		}
		return;
	}
	else
	{
		m_try_req_authenticate_times = 0;
		SendLoginRequest();
	}
}

void traderctp::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	
	if (nullptr != pRspAuthenticateField)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pRspAuthenticateField);
		std::string strMsg = "";
		nss.ToString(&strMsg);
		Log(LOG_WARNING,strMsg.c_str()
			, "fun=OnRspAuthenticate;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast?"true":"false");
	}
	else
	{
		Log(LOG_WARNING,nullptr
			, "fun=OnRspAuthenticate;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	
	//还在等待登录阶段
	if (!m_b_login.load())
	{
		if ((nullptr != pRspInfo) && (pRspInfo->ErrorID != 0))
		{			
			OutputNotifySycn(m_loging_connectId
				, pRspInfo->ErrorID,
				u8"交易服务器认证失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn_status = 0;
			_logInCondition.notify_all();
			return;
		}
		else
		{			
			m_try_req_authenticate_times = 0;
			SendLoginRequest();
		}
	}
	else
	{
		std::shared_ptr<CThostFtdcRspInfoField> ptr = nullptr;
		ptr = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
		_ios.post(boost::bind(&traderctp::ProcessOnRspAuthenticate, this, ptr));
	}
}

void traderctp::ProcessOnRspUserLogin(std::shared_ptr<CThostFtdcRspUserLoginField> pRspUserLogin
	, std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	m_position_ready = false;
	m_req_login_dt.store(0);
	if (nullptr != pRspInfo && pRspInfo->ErrorID != 0)
	{
		OutputNotifyAllSycn(pRspInfo->ErrorID,
			u8"交易服务器重登录失败, " + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
		//如果是未初始化
		if (7 == pRspInfo->ErrorID)
		{
			_ios.post(boost::bind(&traderctp::ReinitCtp, this));
		}
		return;
	}
	else
	{
		m_try_req_login_times = 0;
		std::string trading_day = pRspUserLogin->TradingDay;
		if (m_trading_day != trading_day)
		{
			//一个新交易日的重新连接,需要重新初始化所有变量
			m_ordermap_local_remote.clear();
			m_ordermap_remote_local.clear();

			m_input_order_key_map.clear();
			m_action_order_map.clear();
			m_req_transfer_list.clear();
			m_insert_order_set.clear();
			m_cancel_order_set.clear();

			m_data.m_accounts.clear();
			m_data.m_banks.clear();
			m_data.m_orders.clear();
			m_data.m_positions.clear();
			m_data.m_trades.clear();
			m_data.m_transfers.clear();
			m_data.m_trade_more_data = false;
			m_data.trading_day = trading_day;

			m_banks.clear();

			m_settlement_info = "";

			m_notify_seq = 0;
			m_data_seq = 0;
			_requestID.store(0);

			m_trading_day = "";
			m_front_id = 0;
			m_session_id = 0;
			m_order_ref = 0;

			m_req_login_dt = 0;
			m_next_qry_dt = 0;
			m_next_send_dt = 0;

			m_need_query_settlement.store(false);
			m_confirm_settlement_status.store(0);

			m_req_account_id.store(0);
			m_rsp_account_id.store(0);

			m_req_position_id.store(0);
			m_rsp_position_id.store(0);
			m_position_ready.store(false);
			m_position_inited.store(false);

			m_need_query_bank.store(false);
			m_need_query_register.store(false);

			m_something_changed = false;
			m_peeking_message = false;

			m_need_save_file.store(false);

			m_need_query_broker_trading_params.store(false);
			m_Algorithm_Type = THOST_FTDC_AG_None;

			m_trading_day = trading_day;
			m_front_id = pRspUserLogin->FrontID;
			m_session_id = pRspUserLogin->SessionID;
			m_order_ref = atoi(pRspUserLogin->MaxOrderRef);

			m_is_qry_his_settlement_info.store(false);
			m_his_settlement_info = "";
			m_qry_his_settlement_info_trading_days.clear();

			m_condition_order_task.clear();

			AfterLogin();
		}
		else
		{
			//正常的断开重连成功
			m_front_id = pRspUserLogin->FrontID;
			m_session_id = pRspUserLogin->SessionID;
			m_order_ref = atoi(pRspUserLogin->MaxOrderRef);
			OutputNotifyAllSycn(0, u8"交易服务器重登录成功");

			m_req_position_id++;
			m_req_account_id++;
		}
	}
}

void traderctp::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pRspUserLogin)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pRspUserLogin);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspUserLogin;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspUserLogin;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	//还在等待登录阶段
	if (!m_b_login.load())
	{
		m_position_ready = false;
		m_req_login_dt.store(0);
		if (pRspInfo->ErrorID != 0)
		{
			OutputNotifySycn(m_loging_connectId
				, pRspInfo->ErrorID,
				u8"交易服务器登录失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			if ((pRspInfo->ErrorID == 140)
				|| (pRspInfo->ErrorID == 131)
				|| (pRspInfo->ErrorID == 141))
			{
				_logIn_status = 1;
			}
			else
			{
				_logIn_status = 0;
			}
			_logInCondition.notify_all();
			return;
		}
		else
		{
			m_try_req_login_times = 0;
			std::string trading_day = pRspUserLogin->TradingDay;
			if (m_trading_day != trading_day)
			{
				m_ordermap_local_remote.clear();
				m_ordermap_remote_local.clear();
			}
			m_trading_day = trading_day;
			m_front_id = pRspUserLogin->FrontID;
			m_session_id = pRspUserLogin->SessionID;
			m_order_ref = atoi(pRspUserLogin->MaxOrderRef);
			OutputNotifySycn(m_loging_connectId, 0, u8"登录成功");
			AfterLogin();
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn_status = 2;
			_logInCondition.notify_all();
		}
	}
	else
	{
		std::shared_ptr<CThostFtdcRspUserLoginField> ptr1 = nullptr;
		ptr1 = std::make_shared<CThostFtdcRspUserLoginField>(CThostFtdcRspUserLoginField(*pRspUserLogin));
		std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
		_ios.post(boost::bind(&traderctp::ProcessOnRspUserLogin, this, ptr1, ptr2));
	}
}

void traderctp::ProcessQrySettlementInfoConfirm(std::shared_ptr<CThostFtdcSettlementInfoConfirmField> pSettlementInfoConfirm)
{
	if ((nullptr != pSettlementInfoConfirm)
		&& (std::string(pSettlementInfoConfirm->ConfirmDate) >= m_trading_day))
	{
		//已经确认过结算单
		m_confirm_settlement_status.store(2);
		return;
	}	
	m_need_query_settlement.store(true);
	m_confirm_settlement_status.store(0);
}

void traderctp::OnRspQrySettlementInfoConfirm(
	CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pSettlementInfoConfirm)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pSettlementInfoConfirm);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQrySettlementInfoConfirm;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQrySettlementInfoConfirm;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcSettlementInfoConfirmField> ptr = nullptr;
	if (nullptr != pSettlementInfoConfirm)
	{
		ptr = std::make_shared<CThostFtdcSettlementInfoConfirmField>(
			CThostFtdcSettlementInfoConfirmField(*pSettlementInfoConfirm));
	}
	_ios.post(boost::bind(&traderctp::ProcessQrySettlementInfoConfirm, this, ptr));
}

void  traderctp::ProcessQrySettlementInfo(std::shared_ptr<CThostFtdcSettlementInfoField> pSettlementInfo, bool bIsLast)
{
	if (m_is_qry_his_settlement_info.load())
	{
		if (bIsLast)
		{
			m_is_qry_his_settlement_info.store(false);
			std::string str = GBKToUTF8(pSettlementInfo->Content);
			m_his_settlement_info += str;
			NotifyClientHisSettlementInfo(m_his_settlement_info);
		}
		else
		{
			std::string str = GBKToUTF8(pSettlementInfo->Content);
			m_his_settlement_info += str;
		}
	}
	else
	{
		if (bIsLast)
		{
			m_need_query_settlement.store(false);
			std::string str = GBKToUTF8(pSettlementInfo->Content);
			m_settlement_info += str;
			if (0 == m_confirm_settlement_status.load())
			{
				OutputNotifyAllSycn(0, m_settlement_info, "INFO", "SETTLEMENT");
			}
		}
		else
		{
			std::string str = GBKToUTF8(pSettlementInfo->Content);
			m_settlement_info += str;
		}
	}	
}

void traderctp::ProcessEmptySettlementInfo()
{	
	if (m_is_qry_his_settlement_info.load())
	{
		m_is_qry_his_settlement_info.store(false);
		NotifyClientHisSettlementInfo("");
	}
	else
	{
		m_need_query_settlement.store(false);
		if (0 == m_confirm_settlement_status.load())
		{
			OutputNotifyAllSycn(0, "", "INFO", "SETTLEMENT");
		}
	}	
}

void traderctp::NotifyClientHisSettlementInfo(const std::string& hisSettlementInfo)
{
	if (m_qry_his_settlement_info_trading_days.empty())
	{
		return;
	}
	int trading_day = m_qry_his_settlement_info_trading_days.front();
	m_qry_his_settlement_info_trading_days.pop_front();

	//构建数据包
	qry_settlement_info settle;
	settle.aid = "qry_settlement_info";
	settle.trading_day = trading_day;
	settle.user_name = _req_login.user_name;
	settle.settlement_info = hisSettlementInfo;

	SerializerTradeBase nss;
	nss.FromVar(settle);
	std::string strMsg = "";
	nss.ToString(&strMsg);
	std::string str = GetConnectionStr();
	if (!str.empty())
	{
		std::shared_ptr<std::string> msg_ptr(new std::string(strMsg));
		std::shared_ptr<std::string> conn_ptr(new std::string(str));
		_ios.post(boost::bind(&traderctp::SendMsgAll, this, conn_ptr, msg_ptr));
	}	
}

void traderctp::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pSettlementInfo)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pSettlementInfo);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQrySettlementInfo;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQrySettlementInfo;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	if (nullptr == pSettlementInfo)
	{
		if ((nullptr == pRspInfo) && (bIsLast))
		{
			_ios.post(boost::bind(&traderctp::ProcessEmptySettlementInfo, this));
		}
		return;
	}
	else
	{
		std::shared_ptr<CThostFtdcSettlementInfoField> ptr
			= std::make_shared<CThostFtdcSettlementInfoField>
			(CThostFtdcSettlementInfoField(*pSettlementInfo));
		_ios.post(boost::bind(&traderctp::ProcessQrySettlementInfo, this, ptr, bIsLast));
	}
}

void traderctp::ProcessSettlementInfoConfirm(std::shared_ptr<CThostFtdcSettlementInfoConfirmField> pSettlementInfoConfirm
	, bool bIsLast)
{
	if (nullptr == pSettlementInfoConfirm)
	{
		return;
	}
		
	if (bIsLast)
	{
		m_confirm_settlement_status.store(2);
	}
}

void traderctp::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pSettlementInfoConfirm)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pSettlementInfoConfirm);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO,strMsg.c_str()
			, "fun=OnRspSettlementInfoConfirm;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO,nullptr
			, "fun=OnRspSettlementInfoConfirm;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");

		return;
	}

	std::shared_ptr<CThostFtdcSettlementInfoConfirmField> ptr
		= std::make_shared<CThostFtdcSettlementInfoConfirmField>
		(CThostFtdcSettlementInfoConfirmField(*pSettlementInfoConfirm));
	_ios.post(boost::bind(&traderctp::ProcessSettlementInfoConfirm, this, ptr, bIsLast));
}

void traderctp::ProcessUserPasswordUpdateField(std::shared_ptr<CThostFtdcUserPasswordUpdateField> pUserPasswordUpdate,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (nullptr==pRspInfo)
	{
		return;
	}
		
	if (pRspInfo->ErrorID == 0)
	{
		std::string strOldPassword = GBKToUTF8(pUserPasswordUpdate->OldPassword);
		std::string strNewPassword = GBKToUTF8(pUserPasswordUpdate->NewPassword);
		OutputNotifySycn(m_loging_connectId, pRspInfo->ErrorID, u8"修改密码成功");
		if (_req_login.password == strOldPassword)
		{
			_req_login.password = strNewPassword;
		}
	}
	else
	{
		OutputNotifySycn(m_loging_connectId, pRspInfo->ErrorID
			, u8"修改密码失败," + GBKToUTF8(pRspInfo->ErrorMsg)
			, "WARNING");
	}
}

void traderctp::OnRspUserPasswordUpdate(
	CThostFtdcUserPasswordUpdateField *pUserPasswordUpdate
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pUserPasswordUpdate)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pUserPasswordUpdate);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspUserPasswordUpdate;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspUserPasswordUpdate;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcUserPasswordUpdateField> ptr1 = nullptr;
	if (nullptr != pUserPasswordUpdate)
	{
		ptr1 = std::make_shared<CThostFtdcUserPasswordUpdateField>(
			CThostFtdcUserPasswordUpdateField(*pUserPasswordUpdate));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(
			CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessUserPasswordUpdateField
		, this, ptr1, ptr2));

}

void traderctp::ProcessRspOrderInsert(std::shared_ptr<CThostFtdcInputOrderField> pInputOrder
	, std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{	
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		std::stringstream ss;
		ss << m_front_id << m_session_id << pInputOrder->OrderRef;
		std::string strKey = ss.str();
		auto it = m_input_order_key_map.find(strKey);
		if (it != m_input_order_key_map.end())
		{
			//找到委托单
			RemoteOrderKey remote_key;
			remote_key.exchange_id = pInputOrder->ExchangeID;
			remote_key.instrument_id = pInputOrder->InstrumentID;
			remote_key.front_id = m_front_id;
			remote_key.session_id = m_session_id;
			remote_key.order_ref = pInputOrder->OrderRef;

			LocalOrderKey local_key;
			OrderIdRemoteToLocal(remote_key, &local_key);

			Order& order = GetOrder(local_key.order_id);

			//委托单初始属性(由下单者在下单前确定, 不再改变)
			order.seqno = 0;
			order.user_id = local_key.user_id;
			order.order_id = local_key.order_id;
			order.exchange_id = pInputOrder->ExchangeID;
			order.instrument_id = pInputOrder->InstrumentID;
			switch (pInputOrder->Direction)
			{
			case THOST_FTDC_D_Buy:
				order.direction = kDirectionBuy;
				break;
			case THOST_FTDC_D_Sell:
				order.direction = kDirectionSell;
				break;
			default:
				break;
			}
			switch (pInputOrder->CombOffsetFlag[0])
			{
			case THOST_FTDC_OF_Open:
				order.offset = kOffsetOpen;
				break;
			case THOST_FTDC_OF_CloseToday:
				order.offset = kOffsetCloseToday;
				break;
			case THOST_FTDC_OF_Close:
			case THOST_FTDC_OF_CloseYesterday:
			case THOST_FTDC_OF_ForceOff:
			case THOST_FTDC_OF_LocalForceClose:
				order.offset = kOffsetClose;
				break;
			default:
				break;
			}
			order.volume_orign = pInputOrder->VolumeTotalOriginal;
			switch (pInputOrder->OrderPriceType)
			{
			case THOST_FTDC_OPT_AnyPrice:
				order.price_type = kPriceTypeAny;
				break;
			case THOST_FTDC_OPT_LimitPrice:
				order.price_type = kPriceTypeLimit;
				break;
			case THOST_FTDC_OPT_BestPrice:
				order.price_type = kPriceTypeBest;
				break;
			case THOST_FTDC_OPT_FiveLevelPrice:
				order.price_type = kPriceTypeFiveLevel;
				break;
			default:
				break;
			}
			order.limit_price = pInputOrder->LimitPrice;
			switch (pInputOrder->TimeCondition)
			{
			case THOST_FTDC_TC_IOC:
				order.time_condition = kOrderTimeConditionIOC;
				break;
			case THOST_FTDC_TC_GFS:
				order.time_condition = kOrderTimeConditionGFS;
				break;
			case THOST_FTDC_TC_GFD:
				order.time_condition = kOrderTimeConditionGFD;
				break;
			case THOST_FTDC_TC_GTD:
				order.time_condition = kOrderTimeConditionGTD;
				break;
			case THOST_FTDC_TC_GTC:
				order.time_condition = kOrderTimeConditionGTC;
				break;
			case THOST_FTDC_TC_GFA:
				order.time_condition = kOrderTimeConditionGFA;
				break;
			default:
				break;
			}
			switch (pInputOrder->VolumeCondition)
			{
			case THOST_FTDC_VC_AV:
				order.volume_condition = kOrderVolumeConditionAny;
				break;
			case THOST_FTDC_VC_MV:
				order.volume_condition = kOrderVolumeConditionMin;
				break;
			case THOST_FTDC_VC_CV:
				order.volume_condition = kOrderVolumeConditionAll;
				break;
			default:
				break;
			}
			//委托单当前状态
			order.volume_left = pInputOrder->VolumeTotalOriginal;
			order.status = kOrderStatusFinished;
			order.last_msg = GBKToUTF8(pRspInfo->ErrorMsg);
			order.changed = true;
			m_something_changed = true;
			SendUserData();

			OutputNotifyAllSycn(pRspInfo->ErrorID
				, u8"下单失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");

			m_input_order_key_map.erase(it);
		}
	}
}

void traderctp::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pInputOrder)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pInputOrder);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspOrderInsert;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspOrderInsert;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcInputOrderField> ptr1 = nullptr;
	if (nullptr != pInputOrder)
	{
		ptr1 = std::make_shared<CThostFtdcInputOrderField>(CThostFtdcInputOrderField(*pInputOrder));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessRspOrderInsert, this, ptr1, ptr2));
}


void traderctp::ProcessOrderAction(std::shared_ptr<CThostFtdcInputOrderActionField> pInputOrderAction,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (pRspInfo->ErrorID != 0)
	{
		OutputNotifyAllSycn(pRspInfo->ErrorID
			, u8"撤单失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
	}
}

void traderctp::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pInputOrderAction)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pInputOrderAction);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspOrderAction;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspOrderAction;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcInputOrderActionField> ptr1 = nullptr;
	if (nullptr != pInputOrderAction)
	{
		ptr1 = std::make_shared<CThostFtdcInputOrderActionField>(CThostFtdcInputOrderActionField(*pInputOrderAction));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessOrderAction, this, ptr1, ptr2));
}

void traderctp::ProcessErrRtnOrderInsert(std::shared_ptr<CThostFtdcInputOrderField> pInputOrder,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (pInputOrder && pRspInfo && pRspInfo->ErrorID != 0)
	{
		std::stringstream ss;
		ss << m_front_id << m_session_id << pInputOrder->OrderRef;
		std::string strKey = ss.str();
		auto it = m_input_order_key_map.find(strKey);
		if (it != m_input_order_key_map.end())
		{
			OutputNotifyAllSycn(pRspInfo->ErrorID
				, u8"下单失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
			m_input_order_key_map.erase(it);

			//找到委托单
			RemoteOrderKey remote_key;
			remote_key.exchange_id = pInputOrder->ExchangeID;
			remote_key.instrument_id = pInputOrder->InstrumentID;
			remote_key.front_id = m_front_id;
			remote_key.session_id = m_session_id;
			remote_key.order_ref = pInputOrder->OrderRef;

			LocalOrderKey local_key;
			OrderIdRemoteToLocal(remote_key, &local_key);

			Order& order = GetOrder(local_key.order_id);

			//委托单初始属性(由下单者在下单前确定, 不再改变)
			order.seqno = 0;
			order.user_id = local_key.user_id;
			order.order_id = local_key.order_id;
			order.exchange_id = pInputOrder->ExchangeID;
			order.instrument_id = pInputOrder->InstrumentID;

			switch (pInputOrder->Direction)
			{
			case THOST_FTDC_D_Buy:
				order.direction = kDirectionBuy;
				break;
			case THOST_FTDC_D_Sell:
				order.direction = kDirectionSell;
				break;
			default:
				break;
			}

			switch (pInputOrder->CombOffsetFlag[0])
			{
			case THOST_FTDC_OF_Open:
				order.offset = kOffsetOpen;
				break;
			case THOST_FTDC_OF_CloseToday:
				order.offset = kOffsetCloseToday;
				break;
			case THOST_FTDC_OF_Close:
			case THOST_FTDC_OF_CloseYesterday:
			case THOST_FTDC_OF_ForceOff:
			case THOST_FTDC_OF_LocalForceClose:
				order.offset = kOffsetClose;
				break;
			default:
				break;
			}

			order.volume_orign = pInputOrder->VolumeTotalOriginal;
			switch (pInputOrder->OrderPriceType)
			{
			case THOST_FTDC_OPT_AnyPrice:
				order.price_type = kPriceTypeAny;
				break;
			case THOST_FTDC_OPT_LimitPrice:
				order.price_type = kPriceTypeLimit;
				break;
			case THOST_FTDC_OPT_BestPrice:
				order.price_type = kPriceTypeBest;
				break;
			case THOST_FTDC_OPT_FiveLevelPrice:
				order.price_type = kPriceTypeFiveLevel;
				break;
			default:
				break;
			}

			order.limit_price = pInputOrder->LimitPrice;
			switch (pInputOrder->TimeCondition)
			{
			case THOST_FTDC_TC_IOC:
				order.time_condition = kOrderTimeConditionIOC;
				break;
			case THOST_FTDC_TC_GFS:
				order.time_condition = kOrderTimeConditionGFS;
				break;
			case THOST_FTDC_TC_GFD:
				order.time_condition = kOrderTimeConditionGFD;
				break;
			case THOST_FTDC_TC_GTD:
				order.time_condition = kOrderTimeConditionGTD;
				break;
			case THOST_FTDC_TC_GTC:
				order.time_condition = kOrderTimeConditionGTC;
				break;
			case THOST_FTDC_TC_GFA:
				order.time_condition = kOrderTimeConditionGFA;
				break;
			default:
				break;
			}

			switch (pInputOrder->VolumeCondition)
			{
			case THOST_FTDC_VC_AV:
				order.volume_condition = kOrderVolumeConditionAny;
				break;
			case THOST_FTDC_VC_MV:
				order.volume_condition = kOrderVolumeConditionMin;
				break;
			case THOST_FTDC_VC_CV:
				order.volume_condition = kOrderVolumeConditionAll;
				break;
			default:
				break;
			}

			//委托单当前状态
			order.volume_left = pInputOrder->VolumeTotalOriginal;
			order.status = kOrderStatusFinished;
			order.last_msg = GBKToUTF8(pRspInfo->ErrorMsg);
			order.changed = true;
			m_something_changed = true;
			SendUserData();
		}
	}
}

void traderctp::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder
	, CThostFtdcRspInfoField *pRspInfo)
{
	if (nullptr != pInputOrder)
	{
		std::stringstream ss;
		ss << pInputOrder->InstrumentID
			<< pInputOrder->OrderRef
			<< pInputOrder->OrderPriceType
			<< pInputOrder->CombOffsetFlag
			<< pInputOrder->LimitPrice
			<< pInputOrder->VolumeTotalOriginal
			<< pInputOrder->TimeCondition
			<< pInputOrder->VolumeCondition;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it = m_err_rtn_order_insert_log_map.find(strKey);
		if (it == m_err_rtn_order_insert_log_map.end())
		{
			m_err_rtn_order_insert_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));

			SerializerLogCtp nss;
			nss.FromVar(*pInputOrder);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnErrRtnOrderInsert;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, pRspInfo ? pRspInfo->ErrorID : -999
				, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : "");

		}
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnErrRtnOrderInsert;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : "");
	}

	std::shared_ptr<CThostFtdcInputOrderField> ptr1 = nullptr;
	if (nullptr != pInputOrder)
	{
		ptr1 = std::make_shared<CThostFtdcInputOrderField>
			(CThostFtdcInputOrderField(*pInputOrder));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessErrRtnOrderInsert
		, this, ptr1, ptr2));
}

void traderctp::ProcessErrRtnOrderAction(std::shared_ptr<CThostFtdcOrderActionField> pOrderAction,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (pOrderAction && pRspInfo && pRspInfo->ErrorID != 0)
	{
		std::stringstream ss;
		ss << pOrderAction->FrontID << pOrderAction->SessionID << pOrderAction->OrderRef;
		std::string strKey = ss.str();
		auto it = m_action_order_map.find(strKey);
		if (it != m_action_order_map.end())
		{
			OutputNotifyAllSycn(pRspInfo->ErrorID
				, u8"撤单失败," + GBKToUTF8(pRspInfo->ErrorMsg)
				, "WARNING");
			m_action_order_map.erase(it);
		}
	}
}

void traderctp::OnErrRtnOrderAction(CThostFtdcOrderActionField *pOrderAction
	, CThostFtdcRspInfoField *pRspInfo)
{
	if (nullptr != pOrderAction)
	{
		std::stringstream ss;
		ss << pOrderAction->FrontID
			<< pOrderAction->SessionID
			<< pOrderAction->OrderRef
			<< pOrderAction->OrderActionStatus;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it = m_err_rtn_order_action_log_map.find(strKey);
		if (it == m_err_rtn_order_action_log_map.end())
		{
			m_err_rtn_order_action_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));

			SerializerLogCtp nss;
			nss.FromVar(*pOrderAction);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnErrRtnOrderAction;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, pRspInfo ? pRspInfo->ErrorID : -999
				, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : "");
		}
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnErrRtnOrderAction;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : "");
	}

	std::shared_ptr<CThostFtdcOrderActionField> ptr1 = nullptr;
	if (nullptr != pOrderAction)
	{
		ptr1 = std::make_shared<CThostFtdcOrderActionField>
			(CThostFtdcOrderActionField(*pOrderAction));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessErrRtnOrderAction, this, ptr1, ptr2));
}

void traderctp::ProcessQryInvestorPosition(
	std::shared_ptr<CThostFtdcInvestorPositionField> pRspInvestorPosition,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInvestorPosition)
	{
		std::string exchange_id = GuessExchangeId(pRspInvestorPosition->InstrumentID);
		std::string symbol = exchange_id + "." + pRspInvestorPosition->InstrumentID;
		auto ins = GetInstrument(symbol);
		if (!ins)
		{
			Log(LOG_WARNING, nullptr
				, "fun=ProcessQryInvestorPosition;msg=ctp OnRspQryInvestorPosition,instrument not exist;key=%s;bid=%s;user_name=%s;symbol=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, symbol.c_str());
		}
		else
		{
			bool b_has_td_yd_distinct = (exchange_id == "SHFE") || (exchange_id == "INE");
			Position& position = GetPosition(symbol);
			position.user_id = pRspInvestorPosition->InvestorID;
			position.exchange_id = exchange_id;
			position.instrument_id = pRspInvestorPosition->InstrumentID;
			if (pRspInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
			{
				if (!b_has_td_yd_distinct)
				{
					position.volume_long_yd = pRspInvestorPosition->YdPosition;
				}
				else
				{
					if (pRspInvestorPosition->PositionDate == THOST_FTDC_PSD_History)
					{
						position.volume_long_yd = pRspInvestorPosition->YdPosition;
					}
				}				
				if (pRspInvestorPosition->PositionDate == THOST_FTDC_PSD_Today)
				{
					position.volume_long_today = pRspInvestorPosition->Position;
					position.volume_long_frozen_today = pRspInvestorPosition->ShortFrozen;
					position.position_cost_long_today = pRspInvestorPosition->PositionCost;
					position.open_cost_long_today = pRspInvestorPosition->OpenCost;
					position.margin_long_today = pRspInvestorPosition->UseMargin;
				}
				else
				{
					position.volume_long_his = pRspInvestorPosition->Position;
					position.volume_long_frozen_his = pRspInvestorPosition->ShortFrozen;
					position.position_cost_long_his = pRspInvestorPosition->PositionCost;
					position.open_cost_long_his = pRspInvestorPosition->OpenCost;
					position.margin_long_his = pRspInvestorPosition->UseMargin;
				}
				position.position_cost_long = position.position_cost_long_today + position.position_cost_long_his;
				position.open_cost_long = position.open_cost_long_today + position.open_cost_long_his;
				position.margin_long = position.margin_long_today + position.margin_long_his;
			}
			else
			{
				if (!b_has_td_yd_distinct)
				{
					position.volume_short_yd = pRspInvestorPosition->YdPosition;
				}
				else
				{
					if (pRspInvestorPosition->PositionDate == THOST_FTDC_PSD_History)
					{
						position.volume_short_yd = pRspInvestorPosition->YdPosition;
					}
				}
				if (pRspInvestorPosition->PositionDate == THOST_FTDC_PSD_Today)
				{
					position.volume_short_today = pRspInvestorPosition->Position;
					position.volume_short_frozen_today = pRspInvestorPosition->LongFrozen;
					position.position_cost_short_today = pRspInvestorPosition->PositionCost;
					position.open_cost_short_today = pRspInvestorPosition->OpenCost;
					position.margin_short_today = pRspInvestorPosition->UseMargin;
				}
				else
				{
					position.volume_short_his = pRspInvestorPosition->Position;
					position.volume_short_frozen_his = pRspInvestorPosition->LongFrozen;
					position.position_cost_short_his = pRspInvestorPosition->PositionCost;
					position.open_cost_short_his = pRspInvestorPosition->OpenCost;
					position.margin_short_his = pRspInvestorPosition->UseMargin;
				}
				position.position_cost_short = position.position_cost_short_today + position.position_cost_short_his;
				position.open_cost_short = position.open_cost_short_today + position.open_cost_short_his;
				position.margin_short = position.margin_short_today + position.margin_short_his;
			}
			position.changed = true;
		}
	}
	if (bIsLast)
	{
		m_rsp_position_id.store(nRequestID);
		m_something_changed = true;
		m_position_ready = true;
		if(!m_position_inited)
		{
			InitPositionVolume();
			m_position_inited.store(true);
		}
		SendUserData();
	}
}

void traderctp::InitPositionVolume()
{
	for (auto it = m_data.m_positions.begin();
		it != m_data.m_positions.end(); ++it)
	{
		const std::string& symbol = it->first;
		Position& position = it->second;
		position.pos_long_today = 0;
		position.pos_long_his = position.volume_long_yd;
		position.pos_short_today = 0;
		position.pos_short_his = position.volume_short_yd;
		position.changed=true;
	}
	std::map<long, Trade*> sorted_trade;
	for (auto it = m_data.m_trades.begin();
		it != m_data.m_trades.end(); ++it)
	{
		Trade& trade = it->second;
		sorted_trade[trade.seqno] = &trade;
	}
	for(auto it = sorted_trade.begin(); it!= sorted_trade.end(); ++it)
	{
		Trade& trade = *(it->second);
		AdjustPositionByTrade(trade);
	}
}

void traderctp::AdjustPositionByTrade(const Trade& trade)
{
	Position& pos = GetPosition(trade.symbol());
	if(trade.offset == kOffsetOpen)
	{
		if(trade.direction == kDirectionBuy)
		{
			pos.pos_long_today += trade.volume;
		}
		else
		{
			pos.pos_short_today += trade.volume;
		}
	} 
	else
	{
		if((trade.exchange_id == "SHFE" || trade.exchange_id == "INE") 
			&& trade.offset != kOffsetCloseToday)
		{
			if(trade.direction == kDirectionBuy)
			{
				pos.pos_short_his -= trade.volume;
			}
			else
			{
				pos.pos_long_his -= trade.volume;
			}
		}
		else
		{
			if(trade.direction == kDirectionBuy)
			{
				pos.pos_short_today -= trade.volume;
			}
			else
			{
				pos.pos_long_today -= trade.volume;
			}
		}
		if (pos.pos_short_today + pos.pos_short_his < 0
			||pos.pos_long_today + pos.pos_long_his < 0)
		{
			Log(LOG_ERROR, nullptr
				, "fun=InitPositionVolume;bid=%s;user_name=%s;exchange_id=%s;instrument_id=%s;pos_short_today=%d;pos_short_his=%d;pos_long_today=%d;pos_long_his=%d"
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, trade.exchange_id.c_str()
				, trade.instrument_id.c_str()
				, pos.pos_short_today 
				, pos.pos_short_his
				, pos.pos_long_today 
				, pos.pos_long_his
				);
			return;
		}
		if (pos.pos_short_today < 0)
		{
			pos.pos_short_his += pos.pos_short_today;
			pos.pos_short_today = 0;
		}
		if (pos.pos_short_his < 0)
		{
			pos.pos_short_today += pos.pos_short_his;
			pos.pos_short_his = 0;
		}
		if (pos.pos_long_today < 0)
		{
			pos.pos_long_his += pos.pos_long_today;
			pos.pos_long_today = 0;
		}
		if (pos.pos_long_his < 0)
		{
			pos.pos_long_today += pos.pos_long_his;
			pos.pos_long_his = 0;
		}
	}
	pos.changed = true;
}

void traderctp::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pInvestorPosition)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pInvestorPosition);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQryInvestorPosition;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQryInvestorPosition;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcInvestorPositionField> ptr1 = nullptr;
	if (nullptr != pInvestorPosition)
	{
		ptr1 = std::make_shared<CThostFtdcInvestorPositionField>
			(CThostFtdcInvestorPositionField(*pInvestorPosition));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessQryInvestorPosition
		, this, ptr1, ptr2, nRequestID, bIsLast));

}

void traderctp::ProcessQryBrokerTradingParams(std::shared_ptr<CThostFtdcBrokerTradingParamsField> pBrokerTradingParams,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_need_query_broker_trading_params.store(false);
	}

	if (!pBrokerTradingParams)
	{
		return;
	}

	m_Algorithm_Type = pBrokerTradingParams->Algorithm;
}

void traderctp::OnRspQryBrokerTradingParams(CThostFtdcBrokerTradingParamsField
	*pBrokerTradingParams, CThostFtdcRspInfoField *pRspInfo
	, int nRequestID, bool bIsLast)
{
	if (nullptr != pBrokerTradingParams)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pBrokerTradingParams);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQryBrokerTradingParams;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQryBrokerTradingParams;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcBrokerTradingParamsField> ptr1 = nullptr;
	if (nullptr != pBrokerTradingParams)
	{
		ptr1 = std::make_shared<CThostFtdcBrokerTradingParamsField>
			(CThostFtdcBrokerTradingParamsField(*pBrokerTradingParams));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryBrokerTradingParams, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessQryTradingAccount(std::shared_ptr<CThostFtdcTradingAccountField> pRspInvestorAccount,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_rsp_account_id.store(nRequestID);
	}

	if (nullptr==pRspInvestorAccount)
	{
		return;
	}

	std::string strCurrencyID= GBKToUTF8(pRspInvestorAccount->CurrencyID);

	Account& account = GetAccount(strCurrencyID);

	//账号及币种
	account.user_id = GBKToUTF8(pRspInvestorAccount->AccountID);

	account.currency = strCurrencyID;

	//本交易日开盘前状态
	account.pre_balance = pRspInvestorAccount->PreBalance;

	//本交易日内已发生事件的影响
	account.deposit = pRspInvestorAccount->Deposit;

	account.withdraw = pRspInvestorAccount->Withdraw;

	account.close_profit = pRspInvestorAccount->CloseProfit;

	account.commission = pRspInvestorAccount->Commission;

	account.premium = pRspInvestorAccount->CashIn;

	account.static_balance = pRspInvestorAccount->PreBalance
		- pRspInvestorAccount->PreCredit
		- pRspInvestorAccount->PreMortgage
		+ pRspInvestorAccount->Mortgage
		- pRspInvestorAccount->Withdraw
		+ pRspInvestorAccount->Deposit;

	//当前持仓盈亏
	account.position_profit = pRspInvestorAccount->PositionProfit;

	account.float_profit = 0;
	//当前权益
	account.balance = pRspInvestorAccount->Balance;

	//保证金占用, 冻结及风险度
	account.margin = pRspInvestorAccount->CurrMargin;

	account.frozen_margin = pRspInvestorAccount->FrozenMargin;

	account.frozen_commission = pRspInvestorAccount->FrozenCommission;

	account.frozen_premium = pRspInvestorAccount->FrozenCash;

	account.available = pRspInvestorAccount->Available;

	account.changed = true;
	if (bIsLast)
	{
		m_something_changed = true;
		SendUserData();
	}
}

void traderctp::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pRspInvestorAccount
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pRspInvestorAccount)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pRspInvestorAccount);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQryTradingAccount;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQryTradingAccount;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcTradingAccountField> ptr1 = nullptr;
	if (nullptr != pRspInvestorAccount)
	{
		ptr1 = std::make_shared<CThostFtdcTradingAccountField>(CThostFtdcTradingAccountField(*pRspInvestorAccount));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryTradingAccount, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessQryContractBank(std::shared_ptr<CThostFtdcContractBankField> pContractBank,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (!pContractBank)
	{
		m_need_query_bank.store(false);
		return;
	}

	std::string strBankID = GBKToUTF8(pContractBank->BankID);

	Bank& bank = GetBank(strBankID);
	bank.bank_id = strBankID;
	bank.bank_name = GBKToUTF8(pContractBank->BankName);
	
	if (bIsLast)
	{
		m_need_query_bank.store(false);
	}
}

void traderctp::OnRspQryContractBank(CThostFtdcContractBankField *pContractBank
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pContractBank)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pContractBank);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQryContractBank;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQryContractBank;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcContractBankField> ptr1 = nullptr;
	if (nullptr != pContractBank)
	{
		ptr1 = std::make_shared<CThostFtdcContractBankField>
			(CThostFtdcContractBankField(*pContractBank));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryContractBank, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessQryAccountregister(std::shared_ptr<CThostFtdcAccountregisterField> pAccountregister,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (!pAccountregister)
	{
		m_need_query_register.store(false);
		m_data.m_banks.clear();
		m_data.m_banks = m_banks;
		return;
	}

	std::string strBankID = GBKToUTF8(pAccountregister->BankID);

	Bank& bank = GetBank(strBankID);
	bank.changed = true;
	std::map<std::string, Bank>::iterator it = m_banks.find(bank.bank_id);
	if (it == m_banks.end())
	{
		m_banks.insert(std::map<std::string, Bank>::value_type(bank.bank_id, bank));
	}
	else
	{
		it->second = bank;
	}
	if (bIsLast)
	{
		m_need_query_register.store(false);
		m_data.m_banks.clear();
		m_data.m_banks = m_banks;
		m_something_changed = true;
		SendUserData();
	}
}

void traderctp::OnRspQryAccountregister(CThostFtdcAccountregisterField *pAccountregister
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pAccountregister)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pAccountregister);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQryAccountregister;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQryAccountregister;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcAccountregisterField> ptr1 = nullptr;
	if (nullptr != pAccountregister)
	{
		ptr1 = std::make_shared<CThostFtdcAccountregisterField>(CThostFtdcAccountregisterField(*pAccountregister));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryAccountregister, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessQryTransferSerial(std::shared_ptr<CThostFtdcTransferSerialField> pTransferSerial,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (!pTransferSerial)
	{
		return;
	}
	   
	TransferLog& d = GetTransferLog(std::to_string(pTransferSerial->PlateSerial));

	std::string strCurrencyID = GBKToUTF8(pTransferSerial->CurrencyID);

	d.currency = strCurrencyID;
	d.amount = pTransferSerial->TradeAmount;
	if (pTransferSerial->TradeCode == std::string("202002"))
		d.amount = 0 - d.amount;
	DateTime dt;
	dt.time.microsecond = 0;
	sscanf(pTransferSerial->TradeDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
	sscanf(pTransferSerial->TradeTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
	d.datetime = DateTimeToEpochNano(&dt);
	d.error_id = pTransferSerial->ErrorID;
	d.error_msg = GBKToUTF8(pTransferSerial->ErrorMsg);
	if (bIsLast)
	{
		m_something_changed = true;
		SendUserData();
	}
}

void traderctp::OnRspQryTransferSerial(CThostFtdcTransferSerialField *pTransferSerial
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (nullptr != pTransferSerial)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pTransferSerial);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRspQryTransferSerial;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRspQryTransferSerial;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			, nRequestID
			, bIsLast ? "true" : "false");
	}

	std::shared_ptr<CThostFtdcTransferSerialField> ptr1 = nullptr;
	if (nullptr != pTransferSerial)
	{
		ptr1 = std::make_shared<CThostFtdcTransferSerialField>
			(CThostFtdcTransferSerialField(*pTransferSerial));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryTransferSerial, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessFromBankToFutureByFuture(
	std::shared_ptr<CThostFtdcRspTransferField> pRspTransfer)
{
	if (!pRspTransfer)
	{
		return;
	}

	if (pRspTransfer->ErrorID == 0)
	{
		TransferLog& d = GetTransferLog(std::to_string(pRspTransfer->PlateSerial));

		std::string strCurrencyID = GBKToUTF8(pRspTransfer->CurrencyID);

		d.currency = strCurrencyID;
		d.amount = pRspTransfer->TradeAmount;
		if (pRspTransfer->TradeCode == std::string("202002"))
			d.amount = 0 - d.amount;
		DateTime dt;
		dt.time.microsecond = 0;
		sscanf(pRspTransfer->TradeDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
		sscanf(pRspTransfer->TradeTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
		d.datetime = DateTimeToEpochNano(&dt);
		d.error_id = pRspTransfer->ErrorID;
		d.error_msg = GBKToUTF8(pRspTransfer->ErrorMsg);

		if (!m_req_transfer_list.empty())
		{
			OutputNotifyAllSycn(0, u8"转账成功");
			m_req_transfer_list.pop_front();
		}

		m_something_changed = true;
		SendUserData();
		m_req_account_id++;
	}
	else
	{
		if (!m_req_transfer_list.empty())
		{
			OutputNotifyAllSycn(pRspTransfer->ErrorID
				, u8"银期错误," + GBKToUTF8(pRspTransfer->ErrorMsg)
				, "WARNING");
			m_req_transfer_list.pop_front();
		}

	}
}

void traderctp::OnRtnFromBankToFutureByFuture(
	CThostFtdcRspTransferField *pRspTransfer)
{
	if (nullptr != pRspTransfer)
	{
		std::stringstream ss;
		ss << pRspTransfer->BankSerial
			<< "_" << pRspTransfer->PlateSerial;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it =
			m_rtn_from_bank_to_future_by_future_log_map.find(strKey);
		if (it == m_rtn_from_bank_to_future_by_future_log_map.end())
		{
			m_rtn_from_bank_to_future_by_future_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));

			SerializerLogCtp nss;
			nss.FromVar(*pRspTransfer);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnRtnFromBankToFutureByFuture;key=%s;bid=%s;user_name=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
			);

		}
	}
	else
	{
		Log(LOG_INFO,nullptr
			, "fun=OnRtnFromBankToFutureByFuture;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
		);
	}

	if (nullptr == pRspTransfer)
	{
		return;
	}
	std::shared_ptr<CThostFtdcRspTransferField> ptr1 =
		std::make_shared<CThostFtdcRspTransferField>(
			CThostFtdcRspTransferField(*pRspTransfer));
	_ios.post(boost::bind(&traderctp::ProcessFromBankToFutureByFuture, this
		, ptr1));
}

void traderctp::OnRtnFromFutureToBankByFuture(CThostFtdcRspTransferField *pRspTransfer)
{
	if (nullptr != pRspTransfer)
	{
		std::stringstream ss;
		ss << pRspTransfer->BankSerial
			<< "_" << pRspTransfer->PlateSerial;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it = m_rtn_from_future_to_bank_by_future_log_map.find(strKey);
		if (it == m_rtn_from_future_to_bank_by_future_log_map.end())
		{
			m_rtn_from_future_to_bank_by_future_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));

			SerializerLogCtp nss;
			nss.FromVar(*pRspTransfer);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnRtnFromFutureToBankByFuture;key=%s;bid=%s;user_name=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
			);

		}
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRtnFromFutureToBankByFuture;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
		);
	}

	if (nullptr == pRspTransfer)
	{
		return;
	}
	std::shared_ptr<CThostFtdcRspTransferField> ptr1 =
		std::make_shared<CThostFtdcRspTransferField>(
			CThostFtdcRspTransferField(*pRspTransfer));
	_ios.post(boost::bind(&traderctp::ProcessFromBankToFutureByFuture, this
		, ptr1));
}

void traderctp::ProcessOnErrRtnBankToFutureByFuture(
	std::shared_ptr<CThostFtdcReqTransferField> pReqTransfer
	,std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (nullptr == pReqTransfer)
	{
		return;
	}
	if (nullptr == pRspInfo)
	{
		return;
	}

	if (!m_req_transfer_list.empty())
	{
		OutputNotifyAllAsych(pRspInfo->ErrorID
			, u8"银行资金转期货错误," + GBKToUTF8(pRspInfo->ErrorMsg)
			, "WARNING");
		m_req_transfer_list.pop_front();
	}

}

void traderctp::OnErrRtnBankToFutureByFuture(CThostFtdcReqTransferField *pReqTransfer
	, CThostFtdcRspInfoField *pRspInfo)
{
	if (nullptr != pReqTransfer)
	{
		std::stringstream ss;
		ss << pReqTransfer->BankSerial
			<< "_" << pReqTransfer->PlateSerial;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it = m_err_rtn_bank_to_future_by_future_log_map.find(strKey);
		if (it == m_err_rtn_bank_to_future_by_future_log_map.end())
		{
			m_err_rtn_bank_to_future_by_future_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));

			SerializerLogCtp nss;
			nss.FromVar(*pReqTransfer);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnErrRtnFutureToBankByFuture;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, pRspInfo ? pRspInfo->ErrorID : -999
				, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			);

		}
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnErrRtnBankToFutureByFuture;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			);
	}

	if (nullptr == pRspInfo)
	{
		return;
	}
	
	if (0 == pRspInfo->ErrorID)
	{
		return;
	}

	if (nullptr == pReqTransfer)
	{
		return;
	}

	std::shared_ptr<CThostFtdcReqTransferField> ptr1 =
		std::make_shared<CThostFtdcReqTransferField>(
			CThostFtdcReqTransferField(*pReqTransfer));

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 =
		std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));

	_ios.post(boost::bind(&traderctp::ProcessOnErrRtnBankToFutureByFuture, this
		,ptr1,ptr2));
}

void traderctp::ProcessOnErrRtnFutureToBankByFuture(
	std::shared_ptr<CThostFtdcReqTransferField> pReqTransfer
	,std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (nullptr == pReqTransfer)
	{
		return;
	}
	if (nullptr == pRspInfo)
	{
		return;
	}

	if (!m_req_transfer_list.empty())
	{
		OutputNotifyAllSycn(pRspInfo->ErrorID
			, u8"期货资金转银行错误," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
		m_req_transfer_list.pop_front();
	}

}

void traderctp::OnErrRtnFutureToBankByFuture(CThostFtdcReqTransferField *pReqTransfer, CThostFtdcRspInfoField *pRspInfo)
{
	if (nullptr != pReqTransfer)
	{
		std::stringstream ss;
		ss << pReqTransfer->BankSerial
			<< "_" << pReqTransfer->PlateSerial;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it = m_err_rtn_future_to_bank_by_future_log_map.find(strKey);
		if (it == m_err_rtn_future_to_bank_by_future_log_map.end())
		{
			m_err_rtn_future_to_bank_by_future_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));

			SerializerLogCtp nss;
			nss.FromVar(*pReqTransfer);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnErrRtnFutureToBankByFuture;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, pRspInfo ? pRspInfo->ErrorID : -999
				, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			);
		}
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnErrRtnFutureToBankByFuture;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
		);
	}

	if (nullptr == pRspInfo)
	{
		return;
	}

	if (0 == pRspInfo->ErrorID)
	{
		return;
	}

	if (nullptr == pReqTransfer)
	{
		return;
	}

	std::shared_ptr<CThostFtdcReqTransferField> ptr1 =
		std::make_shared<CThostFtdcReqTransferField>(
			CThostFtdcReqTransferField(*pReqTransfer));

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = std::make_shared<
		CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));

	_ios.post(boost::bind(&traderctp::ProcessOnErrRtnFutureToBankByFuture,this
		, ptr1,ptr2));
}

void traderctp::ProcessRtnOrder(std::shared_ptr<CThostFtdcOrderField> pOrder)
{
	if (nullptr == pOrder)
	{
		return;
	}
	
	std::stringstream ss;
	ss << pOrder->FrontID << pOrder->SessionID << pOrder->OrderRef;
	std::string strKey = ss.str();

	trader_dll::RemoteOrderKey remote_key;
	remote_key.exchange_id = pOrder->ExchangeID;
	remote_key.instrument_id = pOrder->InstrumentID;
	remote_key.front_id = pOrder->FrontID;
	remote_key.session_id = pOrder->SessionID;
	remote_key.order_ref = pOrder->OrderRef;
	remote_key.order_sys_id = pOrder->OrderSysID;
	trader_dll::LocalOrderKey local_key;
	//找到委托单local_key;
	OrderIdRemoteToLocal(remote_key, &local_key);
	Order& order = GetOrder(local_key.order_id);
	//委托单初始属性(由下单者在下单前确定, 不再改变)
	order.seqno = m_data_seq++;
	order.user_id = local_key.user_id;
	order.order_id = local_key.order_id;
	order.exchange_id = pOrder->ExchangeID;
	order.instrument_id = pOrder->InstrumentID;
	auto ins = GetInstrument(order.symbol());
	if (!ins)
	{
		Log(LOG_ERROR,nullptr
			, "fun=ProcessRtnOrder;msg=ctp OnRtnOrder,instrument not exist;key=%s;bid=%s;user_name=%s;symbol=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, order.symbol().c_str());
		return;
	}
	switch (pOrder->Direction)
	{
	case THOST_FTDC_D_Buy:
		order.direction = kDirectionBuy;
		break;
	case THOST_FTDC_D_Sell:
		order.direction = kDirectionSell;
		break;
	default:
		break;
	}
	switch (pOrder->CombOffsetFlag[0])
	{
	case THOST_FTDC_OF_Open:
		order.offset = kOffsetOpen;
		break;
	case THOST_FTDC_OF_CloseToday:
		order.offset = kOffsetCloseToday;
		break;
	case THOST_FTDC_OF_Close:
	case THOST_FTDC_OF_CloseYesterday:
	case THOST_FTDC_OF_ForceOff:
	case THOST_FTDC_OF_LocalForceClose:
		order.offset = kOffsetClose;
		break;
	default:
		break;
	}
	order.volume_orign = pOrder->VolumeTotalOriginal;
	switch (pOrder->OrderPriceType)
	{
	case THOST_FTDC_OPT_AnyPrice:
		order.price_type = kPriceTypeAny;
		break;
	case THOST_FTDC_OPT_LimitPrice:
		order.price_type = kPriceTypeLimit;
		break;
	case THOST_FTDC_OPT_BestPrice:
		order.price_type = kPriceTypeBest;
		break;
	case THOST_FTDC_OPT_FiveLevelPrice:
		order.price_type = kPriceTypeFiveLevel;
		break;
	default:
		break;
	}
	order.limit_price = pOrder->LimitPrice;
	switch (pOrder->TimeCondition)
	{
	case THOST_FTDC_TC_IOC:
		order.time_condition = kOrderTimeConditionIOC;
		break;
	case THOST_FTDC_TC_GFS:
		order.time_condition = kOrderTimeConditionGFS;
		break;
	case THOST_FTDC_TC_GFD:
		order.time_condition = kOrderTimeConditionGFD;
		break;
	case THOST_FTDC_TC_GTD:
		order.time_condition = kOrderTimeConditionGTD;
		break;
	case THOST_FTDC_TC_GTC:
		order.time_condition = kOrderTimeConditionGTC;
		break;
	case THOST_FTDC_TC_GFA:
		order.time_condition = kOrderTimeConditionGFA;
		break;
	default:
		break;
	}
	switch (pOrder->VolumeCondition)
	{
	case THOST_FTDC_VC_AV:
		order.volume_condition = kOrderVolumeConditionAny;
		break;
	case THOST_FTDC_VC_MV:
		order.volume_condition = kOrderVolumeConditionMin;
		break;
	case THOST_FTDC_VC_CV:
		order.volume_condition = kOrderVolumeConditionAll;
		break;
	default:
		break;
	}
	//下单后获得的信息(由期货公司返回, 不会改变)
	DateTime dt;
	dt.time.microsecond = 0;
	sscanf(pOrder->InsertDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
	sscanf(pOrder->InsertTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
	order.insert_date_time = DateTimeToEpochNano(&dt);
	order.exchange_order_id = pOrder->OrderSysID;
	//委托单当前状态
	switch (pOrder->OrderStatus)
	{
	case THOST_FTDC_OST_AllTraded:
	case THOST_FTDC_OST_PartTradedNotQueueing:
	case THOST_FTDC_OST_NoTradeNotQueueing:
	case THOST_FTDC_OST_Canceled:
		order.status = kOrderStatusFinished;
		break;
	case THOST_FTDC_OST_PartTradedQueueing:
	case THOST_FTDC_OST_NoTradeQueueing:
	case THOST_FTDC_OST_Unknown:
		order.status = kOrderStatusAlive;
		break;
	default:
		break;
	}
	order.volume_left = pOrder->VolumeTotal;
	order.last_msg = GBKToUTF8(pOrder->StatusMsg);
	order.changed = true;
	//要求重新查询持仓
	m_req_position_id++;
	m_req_account_id++;
	m_something_changed = true;
	SendUserData();
	//发送下单成功通知
	if (pOrder->OrderStatus != THOST_FTDC_OST_Canceled
		&& pOrder->OrderStatus != THOST_FTDC_OST_Unknown
		&& pOrder->OrderStatus != THOST_FTDC_OST_NoTradeNotQueueing
		&& pOrder->OrderStatus != THOST_FTDC_OST_PartTradedNotQueueing
		)
	{
		auto it = m_insert_order_set.find(pOrder->OrderRef);
		if (it != m_insert_order_set.end())
		{
			m_insert_order_set.erase(it);
			OutputNotifyAllSycn(1, u8"下单成功");
		}

		//更新Order Key				
		auto itOrder = m_input_order_key_map.find(strKey);
		if (itOrder != m_input_order_key_map.end())
		{
			ServerOrderInfo& serverOrderInfo = itOrder->second;
			serverOrderInfo.OrderLocalID = pOrder->OrderLocalID;
			serverOrderInfo.OrderSysID = pOrder->OrderSysID;
		}

	}

	if (pOrder->OrderStatus == THOST_FTDC_OST_Canceled
		&& pOrder->VolumeTotal > 0)
	{
		auto it = m_cancel_order_set.find(order.order_id);
		if (it != m_cancel_order_set.end())
		{
			m_cancel_order_set.erase(it);
			OutputNotifyAllSycn(1, u8"撤单成功");
			CheckConditionOrderCancelOrderTask(order.order_id);
			//删除Order
			auto itOrder = m_input_order_key_map.find(strKey);
			if (itOrder != m_input_order_key_map.end())
			{
				m_input_order_key_map.erase(itOrder);
			}
		}
		else
		{
			auto it2 = m_insert_order_set.find(pOrder->OrderRef);
			if (it2 != m_insert_order_set.end())
			{
				m_insert_order_set.erase(it2);
				OutputNotifyAllSycn(1, u8"下单失败," + order.last_msg, "WARNING");
			}

			//删除Order
			auto itOrder = m_input_order_key_map.find(strKey);
			if (itOrder != m_input_order_key_map.end())
			{
				m_input_order_key_map.erase(itOrder);
			}
		}
	}
}

void traderctp::OnRtnOrder(CThostFtdcOrderField* pOrder)
{
	if (nullptr != pOrder)
	{
		std::stringstream ss;
		ss << pOrder->FrontID
			<< pOrder->SessionID
			<< pOrder->OrderRef
			<< pOrder->OrderSubmitStatus
			<< pOrder->OrderStatus;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it = m_rtn_order_log_map.find(strKey);
		if (it == m_rtn_order_log_map.end())
		{
			m_rtn_order_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));
			SerializerLogCtp nss;
			nss.FromVar(*pOrder);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnRtnOrder;key=%s;bid=%s;user_name=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
			);
		}
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRtnOrder;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()			
		);
	}

	if (nullptr == pOrder)
	{
		return;
	}
	std::shared_ptr<CThostFtdcOrderField> ptr2 =
		std::make_shared<CThostFtdcOrderField>(CThostFtdcOrderField(*pOrder));
	_ios.post(boost::bind(&traderctp::ProcessRtnOrder, this
		, ptr2));
}

void traderctp::ProcessRtnTrade(std::shared_ptr<CThostFtdcTradeField> pTrade)
{
	std::string exchangeId = pTrade->ExchangeID;
	std::string orderSysId = pTrade->OrderSysID;
	for (std::map<std::string, ServerOrderInfo>::iterator it = m_input_order_key_map.begin();
		it != m_input_order_key_map.end(); it++)
	{
		ServerOrderInfo& serverOrderInfo = it->second;
		if ((serverOrderInfo.ExchangeId == exchangeId)
			&& (serverOrderInfo.OrderSysID == orderSysId))
		{
			serverOrderInfo.VolumeLeft -= pTrade->Volume;

			std::stringstream ss;
			ss << u8"成交通知,合约:" << serverOrderInfo.ExchangeId
				<< u8"." << serverOrderInfo.InstrumentId << u8",手数:" << pTrade->Volume ;
			OutputNotifyAllSycn(0, ss.str().c_str());

			if (serverOrderInfo.VolumeLeft <= 0)
			{
				m_input_order_key_map.erase(it);
			}
			break;
		}
	}

	LocalOrderKey local_key;
	FindLocalOrderId(pTrade->ExchangeID, pTrade->OrderSysID, &local_key);
	std::string trade_key = local_key.order_id + "|" + std::string(pTrade->TradeID);
	Trade& trade = GetTrade(trade_key);
	trade.seqno = m_data_seq++;
	trade.trade_id = trade_key;
	trade.user_id = local_key.user_id;
	trade.order_id = local_key.order_id;
	trade.exchange_id = pTrade->ExchangeID;
	trade.instrument_id = pTrade->InstrumentID;
	trade.exchange_trade_id = pTrade->TradeID;
	auto ins = GetInstrument(trade.symbol());
	if (!ins)
	{
		Log(LOG_ERROR,nullptr
			,"fun=ProcessRtnTrade;msg=ctp OnRtnTrade,instrument not exist;key=%s;bid=%s;user_name=%s;symbol=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, trade.symbol().c_str());
		return;
	}

	switch (pTrade->Direction)
	{
	case THOST_FTDC_D_Buy:
		trade.direction = kDirectionBuy;
		break;
	case THOST_FTDC_D_Sell:
		trade.direction = kDirectionSell;
		break;
	default:
		break;
	}

	switch (pTrade->OffsetFlag)
	{
	case THOST_FTDC_OF_Open:
		trade.offset = kOffsetOpen;
		break;
	case THOST_FTDC_OF_CloseToday:
		trade.offset = kOffsetCloseToday;
		break;
	case THOST_FTDC_OF_Close:
	case THOST_FTDC_OF_CloseYesterday:
	case THOST_FTDC_OF_ForceOff:
	case THOST_FTDC_OF_LocalForceClose:
		trade.offset = kOffsetClose;
		break;
	default:
		break;
	}

	trade.volume = pTrade->Volume;
	trade.price = pTrade->Price;
	DateTime dt;
	dt.time.microsecond = 0;

	bool b_is_dce_or_czce = (exchangeId == "CZCE") || (exchangeId == "DCE");
	sscanf(pTrade->TradeTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
	if (b_is_dce_or_czce)
	{
		int nTime = dt.time.hour * 100 + dt.time.minute;
		//夜盘
		if ((nTime > 2030) && (nTime < 2359))
		{
			boost::posix_time::ptime tm = boost::posix_time::second_clock::local_time();
			int nLocalTime = tm.time_of_day().hours() * 100 + tm.time_of_day().minutes();
			//现在还是夜盘时间
			if ((nLocalTime > 2030) && (nLocalTime < 2359))
			{
				dt.date.year = tm.date().year();
				dt.date.month = tm.date().month();
				dt.date.day = tm.date().day();
			}
			//现在已经是白盘时间了
			else
			{
				dt.date.year = tm.date().year();
				dt.date.month = tm.date().month();
				dt.date.day = tm.date().day();
				//跳到上一个工作日
				MoveDateByWorkday(&dt.date, -1);
			}
		}
		//白盘
		else
		{
			sscanf(pTrade->TradeDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
		}
	}
	else
	{
		sscanf(pTrade->TradeDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
	}	
	trade.trade_date_time = DateTimeToEpochNano(&dt);
	trade.commission = 0.0;
	trade.changed = true;
	m_something_changed = true;
	if (m_position_inited)
	{
		AdjustPositionByTrade(trade);
	}
	SendUserData();

	CheckConditionOrderSendOrderTask(trade.order_id);
}

void traderctp::OnRtnTrade(CThostFtdcTradeField* pTrade)
{
	if (nullptr != pTrade)
	{
		std::stringstream ss;
		ss << pTrade->OrderSysID
			<< pTrade->TradeID;
		std::string strKey = ss.str();
		std::map<std::string, std::string>::iterator it = m_rtn_trade_log_map.find(strKey);
		if (it == m_rtn_trade_log_map.end())
		{
			m_rtn_trade_log_map.insert(std::map<std::string, std::string>::value_type(strKey, strKey));

			SerializerLogCtp nss;
			nss.FromVar(*pTrade);
			std::string strMsg = "";
			nss.ToString(&strMsg);

			Log(LOG_INFO, strMsg.c_str()
				, "fun=OnRtnTrade;key=%s;bid=%s;user_name=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
			);
		}
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRtnTrade;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
		);
	}

	if (nullptr == pTrade)
	{
		return;
	}
	std::shared_ptr<CThostFtdcTradeField> ptr2 =
		std::make_shared<CThostFtdcTradeField>(CThostFtdcTradeField(*pTrade));
	_ios.post(boost::bind(&traderctp::ProcessRtnTrade, this, ptr2));
}

void traderctp::ProcessOnRtnTradingNotice(std::shared_ptr<CThostFtdcTradingNoticeInfoField> pTradingNoticeInfo)
{
	if (nullptr == pTradingNoticeInfo)
	{
		return;
	}

	auto s = GBKToUTF8(pTradingNoticeInfo->FieldContent);
	if (!s.empty())
	{		
		OutputNotifyAllAsych(0,s);
	}
}

void traderctp::OnRtnTradingNotice(CThostFtdcTradingNoticeInfoField *pTradingNoticeInfo)
{
	if (nullptr != pTradingNoticeInfo)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pTradingNoticeInfo);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRtnTradingNotice;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
		);
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRtnTradingNotice;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
		);
	}

	if (nullptr == pTradingNoticeInfo)
	{
		return;
	}
	std::shared_ptr<CThostFtdcTradingNoticeInfoField> ptr
		= std::make_shared<CThostFtdcTradingNoticeInfoField>(CThostFtdcTradingNoticeInfoField(*pTradingNoticeInfo));
	_ios.post(boost::bind(&traderctp::ProcessOnRtnTradingNotice, this
		, ptr));
}

void traderctp::OnRspError(CThostFtdcRspInfoField* pRspInfo
	, int nRequestID, bool bIsLast)
{
	Log(LOG_INFO, nullptr
		, "fun=OnRspError;key=%s;bid=%s;user_name=%s;ErrorID=%d;ErrMsg=%s;nRequestID=%d;bIsLast=%s"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999
		, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
		, nRequestID
		, bIsLast ? "true" : "false");	
}

void traderctp::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus)
{
	if (nullptr != pInstrumentStatus)
	{
		SerializerLogCtp nss;
		nss.FromVar(*pInstrumentStatus);
		std::string strMsg = "";
		nss.ToString(&strMsg);

		Log(LOG_INFO, strMsg.c_str()
			, "fun=OnRtnInstrumentStatus;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
		);
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OnRtnInstrumentStatus;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
		);
	}

	if (nullptr == pInstrumentStatus)
	{
		return;
	}

	std::shared_ptr<CThostFtdcInstrumentStatusField> ptr1 =
		std::make_shared<CThostFtdcInstrumentStatusField>(CThostFtdcInstrumentStatusField(*pInstrumentStatus));
	
	_ios.post(boost::bind(&traderctp::ProcessRtnInstrumentStatus,this,ptr1));
}

void traderctp::ProcessRtnInstrumentStatus(std::shared_ptr<CThostFtdcInstrumentStatusField>
	pInstrumentStatus)
{
	//检验状态
	if ((pInstrumentStatus->InstrumentStatus != THOST_FTDC_IS_AuctionOrdering)
		&& (pInstrumentStatus->InstrumentStatus != THOST_FTDC_IS_Continous))
	{
		return;
	}

	//检验时间
	std::string strEnterTime = pInstrumentStatus->EnterTime;
	std::vector<std::string> hms;
	boost::algorithm::split(hms,strEnterTime,boost::algorithm::is_any_of(":"));
	if (hms.size() != 3)
	{
		return;
	}
	int serverTime = atoi(hms[0].c_str()) * 60 * 60 + atoi(hms[1].c_str()) * 60 + atoi(hms[2].c_str());
	boost::posix_time::ptime tm = boost::posix_time::second_clock::local_time();
	int localTime = tm.time_of_day().hours() * 60*60 + tm.time_of_day().minutes()*60+ tm.time_of_day().seconds();
	//服务端时间和客户端时间相差60秒以上,不是正常的状态切换
	if (std::abs(serverTime - localTime) > 60)
	{
		return;
	}

	//检验品种
	std::string strExchangeId = pInstrumentStatus->ExchangeID;
	std::string strInstId = pInstrumentStatus->InstrumentID;
	std::string strSymbolId = strExchangeId + "." + strInstId;
	TInstOrderIdListMap& om = m_condition_order_manager.GetOpenmarketCoMap();
	TInstOrderIdListMap::iterator it = om.find(strSymbolId);
	if (it == om.end())
	{
		return;
	}
		
	m_condition_order_manager.OnMarketOpen(strSymbolId);
}

#pragma endregion


#pragma region ctp_request

int traderctp::ReqUserLogin()
{
	CThostFtdcReqUserLoginField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, _req_login.broker.ctp_broker_id.c_str());
	strcpy_x(field.UserID, _req_login.user_name.c_str());
	strcpy_x(field.Password, _req_login.password.c_str());
	strcpy_x(field.UserProductInfo, _req_login.broker.product_info.c_str());
	strcpy_x(field.LoginRemark, _req_login.client_ip.c_str());
	int ret = m_pTdApi->ReqUserLogin(&field, ++_requestID);
	if (0 != ret)
	{
		Log(LOG_INFO, nullptr
			, "fun=ReqUserLogin;msg=ctp ReqUserLogin fail;key=%s;bid=%s;user_name=%s;UserProductInfo=%s;LoginRemark=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, _req_login.broker.product_info.c_str()
			, _req_login.client_ip.c_str()
			, ret);
	}	
	return ret;
}

void traderctp::SendLoginRequest()
{
	if (m_try_req_login_times > 0)
	{
		int nSeconds = 10 + m_try_req_login_times * 1;
		if (nSeconds > 60)
		{
			nSeconds = 60;
		}
		boost::this_thread::sleep_for(boost::chrono::seconds(nSeconds));
	}
	m_try_req_login_times++;
	Log(LOG_INFO, nullptr
		, "fun=SendLoginRequest;msg=ctp SendLoginRequest;key=%s;bid=%s;user_name=%s;client_app_id=%s;client_system_info_len=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, _req_login.client_app_id.c_str()
		, _req_login.client_system_info.length());
	long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	m_req_login_dt.store(now);
	//提交终端信息
	if ((!_req_login.client_system_info.empty())
		&& (_req_login.bid.find("simnow", 0) == std::string::npos))
	{
		int ret = RegSystemInfo();
		if (0 != ret)
		{
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn_status = 0;
			_logInCondition.notify_all();
			return;
		}
		else
		{
			ret = ReqUserLogin();
			if (0 != ret)
			{
				boost::unique_lock<boost::mutex> lock(_logInmutex);
				_logIn_status = 0;
				_logInCondition.notify_all();
				return;
			}
		}
	}
	else
	{
		int ret = ReqUserLogin();
		if (0 != ret)
		{
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn_status = 0;
			_logInCondition.notify_all();
			return;
		}
	}
}

void traderctp::ReinitCtp()
{
	Log(LOG_INFO,nullptr
		, "fun=ReinitCtp;key=%s;bid=%s;user_name=%s"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str());
	if (nullptr != m_pTdApi)
	{
		StopTdApi();
	}
	boost::this_thread::sleep_for(boost::chrono::seconds(60));
	InitTdApi();
	if (nullptr != m_pTdApi)
	{
		m_pTdApi->Init();
	}
	Log(LOG_INFO,nullptr
		, "fun=ReinitCtp;msg=ctp ReinitCtp end;key=%s;bid=%s;user_name=%s"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str());
}

void traderctp::ReqConfirmSettlement()
{
	CThostFtdcSettlementInfoConfirmField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	int r = m_pTdApi->ReqSettlementInfoConfirm(&field, 0);
	Log(LOG_INFO,nullptr
		,"fun=ReqConfirmSettlement;msg=ctp ReqConfirmSettlement;key=%s;bid=%s;user_name=%s;ret=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, r);
}

void traderctp::ReqQrySettlementInfoConfirm()
{
	CThostFtdcQrySettlementInfoConfirmField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	strcpy_x(field.AccountID, _req_login.user_name.c_str());
	strcpy_x(field.CurrencyID, "CNY");
	int r = m_pTdApi->ReqQrySettlementInfoConfirm(&field, 0);
	Log(LOG_INFO,nullptr
		,"fun=ReqQrySettlementInfoConfirm;msg=ctp ReqQrySettlementInfoConfirm;key=%s;bid=%s;user_name=%s;ret=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, r);
}

int traderctp::ReqQryBrokerTradingParams()
{
	CThostFtdcQryBrokerTradingParamsField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	strcpy_x(field.CurrencyID, "CNY");	
	int r = m_pTdApi->ReqQryBrokerTradingParams(&field, 0);
	if (0 != r)
	{
		Log(LOG_INFO,nullptr
			, "fun=ReqQryBrokerTradingParams;msg=ctp ReqQryBrokerTradingParams;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
	}
	return r;
}

int traderctp::ReqQryAccount(int reqid)
{
	CThostFtdcQryTradingAccountField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	int r = m_pTdApi->ReqQryTradingAccount(&field, reqid);
	if (0 != r)
	{
		Log(LOG_INFO,nullptr
			, "fun=ReqQryAccount;msg=ctp ReqQryTradingAccount;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
	}
	return r;
}

int traderctp::ReqQryPosition(int reqid)
{
	CThostFtdcQryInvestorPositionField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	int r = m_pTdApi->ReqQryInvestorPosition(&field, reqid);
	if (0 != r)
	{
		Log(LOG_INFO, nullptr
			, "fun=ReqQryPosition;msg=ctp ReqQryInvestorPosition;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
	}	
	return r;
}

void traderctp::ReqQryBank()
{
	CThostFtdcQryContractBankField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	m_pTdApi->ReqQryContractBank(&field, 0);
	int r = m_pTdApi->ReqQryContractBank(&field, 0);
	if (0 != r)
	{
		Log(LOG_INFO, nullptr
			, "fun=ReqQryBank;msg=ctp ReqQryContractBank;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
	}	
}

void traderctp::ReqQryAccountRegister()
{
	CThostFtdcQryAccountregisterField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	m_pTdApi->ReqQryAccountregister(&field, 0);
	int r = m_pTdApi->ReqQryAccountregister(&field, 0);
	if (0 != r)
	{
		Log(LOG_INFO, nullptr
			, "fun=ReqQryAccountRegister;msg=ctp ReqQryAccountregister;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
	}	
}

void traderctp::ReqQrySettlementInfo()
{
	CThostFtdcQrySettlementInfoField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	strcpy_x(field.AccountID, _req_login.user_name.c_str());
	int r = m_pTdApi->ReqQrySettlementInfo(&field, 0);
	if (0 != r)
	{
		Log(LOG_INFO, nullptr
			, "fun=ReqQrySettlementInfo;msg=ctp ReqQrySettlementInfo;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
	}	
}

void traderctp::ReqQryHistorySettlementInfo()
{
	if (m_qry_his_settlement_info_trading_days.empty())
	{
		return;
	}

	if (m_is_qry_his_settlement_info)
	{
		return;
	}

	std::string tradingDay = std::to_string(
		m_qry_his_settlement_info_trading_days.front());
	CThostFtdcQrySettlementInfoField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	strcpy_x(field.AccountID, _req_login.user_name.c_str());
	strcpy_x(field.TradingDay, tradingDay.c_str());
	int r = m_pTdApi->ReqQrySettlementInfo(&field, 0);
	if (0 != r)
	{
		Log(LOG_INFO, nullptr
			, "fun=ReqQryHistorySettlementInfo;msg=ctp ReqQryHistorySettlementInfo;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
	}	
	if (r == 0)
	{
		m_his_settlement_info = "";
		m_is_qry_his_settlement_info = true;
	}
}

void traderctp::ReSendSettlementInfo(int connectId)
{
	if (m_need_query_settlement.load())
	{
		return;
	}

	if (m_confirm_settlement_status.load() != 0)
	{
		return;
	}

	OutputNotifySycn(connectId, 0, m_settlement_info, "INFO", "SETTLEMENT");
}

#pragma endregion


#pragma region businesslogic

bool traderctp::OrderIdLocalToRemote(const LocalOrderKey& local_order_key
	, RemoteOrderKey* remote_order_key)
{
	if (local_order_key.order_id.empty())
	{
		remote_order_key->session_id = m_session_id;
		remote_order_key->front_id = m_front_id;
		remote_order_key->order_ref = std::to_string(m_order_ref++);
		return false;
	}
	auto it = m_ordermap_local_remote.find(local_order_key);
	if (it == m_ordermap_local_remote.end())
	{
		remote_order_key->session_id = m_session_id;
		remote_order_key->front_id = m_front_id;
		remote_order_key->order_ref = std::to_string(m_order_ref++);
		m_ordermap_local_remote[local_order_key] = *remote_order_key;
		m_ordermap_remote_local[*remote_order_key] = local_order_key;
		return false;
	}
	else
	{
		*remote_order_key = it->second;
		return true;
	}
}

void traderctp::OrderIdRemoteToLocal(const RemoteOrderKey& remote_order_key
	, LocalOrderKey* local_order_key)
{
	auto it = m_ordermap_remote_local.find(remote_order_key);
	if (it == m_ordermap_remote_local.end())
	{
		char buf[1024];
		sprintf(buf, "SERVER.%s.%08x.%d"
			, remote_order_key.order_ref.c_str()
			, remote_order_key.session_id
			, remote_order_key.front_id);
		local_order_key->order_id = buf;
		local_order_key->user_id = _req_login.user_name;
		m_ordermap_local_remote[*local_order_key] = remote_order_key;
		m_ordermap_remote_local[remote_order_key] = *local_order_key;
	}
	else
	{
		*local_order_key = it->second;
		RemoteOrderKey& r = const_cast<RemoteOrderKey&>(it->first);
		if (!remote_order_key.order_sys_id.empty())
		{
			r.order_sys_id = remote_order_key.order_sys_id;
		}
	}
}

void traderctp::FindLocalOrderId(const std::string& exchange_id
	, const std::string& order_sys_id, LocalOrderKey* local_order_key)
{
	for (auto it = m_ordermap_remote_local.begin()
		; it != m_ordermap_remote_local.end(); ++it)
	{
		const RemoteOrderKey& rkey = it->first;
		if (rkey.order_sys_id == order_sys_id
			&& rkey.exchange_id == exchange_id)
		{
			*local_order_key = it->second;
			return;
		}
	}
}

Order& traderctp::GetOrder(const std::string order_id)
{
	return m_data.m_orders[order_id];
}

Account& traderctp::GetAccount(const std::string account_key)
{
	return m_data.m_accounts[account_key];
}

Position& traderctp::GetPosition(const std::string symbol)
{
	Position& position = m_data.m_positions[symbol];
	return position;
}

Bank& traderctp::GetBank(const std::string& bank_id)
{
	return m_data.m_banks[bank_id];
}

Trade& traderctp::GetTrade(const std::string trade_key)
{
	return m_data.m_trades[trade_key];
}

TransferLog& traderctp::GetTransferLog(const std::string& seq_id)
{
	return m_data.m_transfers[seq_id];
}

void traderctp::LoadFromFile()
{	
	if (m_user_file_path.empty())
	{
		return;
	}
	std::string fn = m_user_file_path + "/" + _key;
	SerializerCtp s;
	if (s.FromFile(fn.c_str()))
	{
		OrderKeyFile kf;
		s.ToVar(kf);
		for (auto it = kf.items.begin(); it != kf.items.end(); ++it)
		{
			m_ordermap_local_remote[it->local_key] = it->remote_key;
			m_ordermap_remote_local[it->remote_key] = it->local_key;
		}
		m_trading_day = kf.trading_day;
	}
}

void traderctp::SaveToFile()
{
	if (m_user_file_path.empty())
	{
		return;
	}

	SerializerCtp s;
	OrderKeyFile kf;
	kf.trading_day = m_trading_day;

	for (auto it = m_ordermap_local_remote.begin();
		it != m_ordermap_local_remote.end(); ++it)
	{
		OrderKeyPair item;
		item.local_key = it->first;
		item.remote_key = it->second;
		kf.items.push_back(item);
	}
	s.FromVar(kf);
	std::string fn = m_user_file_path + "/" + _key;
	s.ToFile(fn.c_str());
}

bool traderctp::NeedReset()
{
	if (m_req_login_dt == 0)
		return false;
	long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (now > m_req_login_dt + 60)
		return true;
	return false;
};

void traderctp::OnIdle()
{
	if (!m_b_login)
	{
		return;
	}

	CheckTimeConditionOrder();

	CheckPriceConditionOrder();

	if (m_need_save_file.load())
	{
		this->SaveToFile();
		m_need_save_file.store(false);
	}

	//有空的时候, 标记为需查询的项, 如果离上次查询时间够远, 应该发起查询
	long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (m_peeking_message && (m_next_send_dt < now))
	{
		m_next_send_dt = now + 100;
		SendUserData();
	}

	if (m_next_qry_dt >= now)
	{
		return;
	}

	if (m_req_position_id > m_rsp_position_id)
	{
		ReqQryPosition(m_req_position_id);
		m_next_qry_dt = now + 1100;
		return;
	}

	if (m_need_query_broker_trading_params)
	{
		ReqQryBrokerTradingParams();
		m_next_qry_dt = now + 1100;
		return;
	}

	if (m_req_account_id > m_rsp_account_id)
	{
		ReqQryAccount(m_req_account_id);
		m_next_qry_dt = now + 1100;
		return;
	}

	if (m_need_query_settlement.load())
	{
		ReqQrySettlementInfo();
		m_next_qry_dt = now + 1100;
		return;
	}

	if (m_need_query_bank.load())
	{
		ReqQryBank();
		m_next_qry_dt = now + 1100;
		return;
	}

	if (m_need_query_register.load())
	{
		ReqQryAccountRegister();
		m_next_qry_dt = now + 1100;
		return;
	}

	if (m_confirm_settlement_status.load() == 1)
	{
		ReqConfirmSettlement();
		m_next_qry_dt = now + 1100;
		return;
	}

	if (!m_qry_his_settlement_info_trading_days.empty())
	{
		ReqQryHistorySettlementInfo();
		m_next_qry_dt = now + 1100;
		return;
	}
}

void traderctp::SendUserData()
{
	if (!m_peeking_message)
	{
		return;
	}

	if (m_data.m_accounts.size() == 0)
		return;

	if (!m_position_ready)
		return;

	//重算所有持仓项的持仓盈亏和浮动盈亏
	double total_position_profit = 0;
	double total_float_profit = 0;
	for (auto it = m_data.m_positions.begin();
		it != m_data.m_positions.end(); ++it)
	{
		const std::string& symbol = it->first;
		Position& ps = it->second;
		if (nullptr == ps.ins)
		{
			ps.ins = GetInstrument(symbol);
		}
		if (nullptr == ps.ins)
		{
			Log(LOG_ERROR,nullptr
				,"fun=SendUserData;msg=ctp miss symbol %s when processing position;key=%s;bid=%s;user_name=%s"
				, symbol.c_str()
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
			);
			continue;
		}
		ps.volume_long = ps.volume_long_his + ps.volume_long_today;
		ps.volume_short = ps.volume_short_his + ps.volume_short_today;
		ps.volume_long_frozen = ps.volume_long_frozen_today + ps.volume_long_frozen_his;
		ps.volume_short_frozen = ps.volume_short_frozen_today + ps.volume_short_frozen_his;
		ps.margin = ps.margin_long + ps.margin_short;
		double last_price = ps.ins->last_price;
		if (!IsValid(last_price))
			last_price = ps.ins->pre_settlement;
		if (IsValid(last_price) && (last_price != ps.last_price || ps.changed))
		{
			ps.last_price = last_price;
			ps.position_profit_long = ps.last_price * ps.volume_long * ps.ins->volume_multiple - ps.position_cost_long;
			ps.position_profit_short = ps.position_cost_short - ps.last_price * ps.volume_short * ps.ins->volume_multiple;
			ps.position_profit = ps.position_profit_long + ps.position_profit_short;
			ps.float_profit_long = ps.last_price * ps.volume_long * ps.ins->volume_multiple - ps.open_cost_long;
			ps.float_profit_short = ps.open_cost_short - ps.last_price * ps.volume_short * ps.ins->volume_multiple;
			ps.float_profit = ps.float_profit_long + ps.float_profit_short;
			if (ps.volume_long > 0)
			{
				ps.open_price_long = ps.open_cost_long / (ps.volume_long * ps.ins->volume_multiple);
				ps.position_price_long = ps.position_cost_long / (ps.volume_long * ps.ins->volume_multiple);
			}
			if (ps.volume_short > 0)
			{
				ps.open_price_short = ps.open_cost_short / (ps.volume_short * ps.ins->volume_multiple);
				ps.position_price_short = ps.position_cost_short / (ps.volume_short * ps.ins->volume_multiple);
			}
			ps.changed = true;
			m_something_changed = true;
		}
		if (IsValid(ps.position_profit))
			total_position_profit += ps.position_profit;
		if (IsValid(ps.float_profit))
			total_float_profit += ps.float_profit;
	}
	//重算资金账户
	if (m_something_changed)
	{
		Account& acc = GetAccount("CNY");
		double dv = total_position_profit - acc.position_profit;
		double po_ori = 0;
		double po_curr = 0;
		double av_diff = 0;		
		switch (m_Algorithm_Type)
		{
		case THOST_FTDC_AG_All:
			po_ori = acc.position_profit;
			po_curr = total_position_profit;
			break;
		case THOST_FTDC_AG_OnlyLost:
			if (acc.position_profit < 0)
			{
				po_ori = acc.position_profit;
			}
			if (total_position_profit < 0)
			{
				po_curr = total_position_profit;
			}
			break;
		case THOST_FTDC_AG_OnlyGain:
			if (acc.position_profit > 0)
			{
				po_ori = acc.position_profit;
			}
			if (total_position_profit > 0)
			{
				po_curr = total_position_profit;
			}
			break;
		case THOST_FTDC_AG_None:
			po_ori = 0;
			po_curr = 0;
			break;
		default:
			break;
		}
		av_diff = po_curr - po_ori;
		acc.position_profit = total_position_profit;
		acc.float_profit = total_float_profit;
		acc.available += av_diff;
		acc.balance += dv;
		if (IsValid(acc.available) && IsValid(acc.balance) && !IsZero(acc.balance))
			acc.risk_ratio = 1.0 - acc.available / acc.balance;
		else
			acc.risk_ratio = NAN;
		acc.changed = true;
	}
	if (!m_something_changed)
		return;
	//构建数据包	
	m_data.m_trade_more_data = false;
	SerializerTradeBase nss;
	rapidjson::Pointer("/aid").Set(*nss.m_doc, "rtn_data");
	rapidjson::Value node_data;
	nss.FromVar(m_data, &node_data);
	rapidjson::Value node_user_id;
	node_user_id.SetString(_req_login.user_name, nss.m_doc->GetAllocator());
	rapidjson::Value node_user;
	node_user.SetObject();
	node_user.AddMember(node_user_id, node_data, nss.m_doc->GetAllocator());
	rapidjson::Pointer("/data/0/trade").Set(*nss.m_doc, node_user);
	std::string json_str;
	nss.ToString(&json_str);
	//发送		
	std::string str = GetConnectionStr();
	if (!str.empty())
	{
		std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
		std::shared_ptr<std::string> conn_ptr(new std::string(str));
		_ios.post(boost::bind(&traderctp::SendMsgAll, this, conn_ptr, msg_ptr));
	}
	m_something_changed = false;
	m_peeking_message = false;
}

void traderctp::SendUserDataImd(int connectId)
{
	//重算所有持仓项的持仓盈亏和浮动盈亏
	double total_position_profit = 0;
	double total_float_profit = 0;
	for (auto it = m_data.m_positions.begin();
		it != m_data.m_positions.end(); ++it)
	{
		const std::string& symbol = it->first;
		Position& ps = it->second;
		if (nullptr == ps.ins)
		{
			ps.ins = GetInstrument(symbol);
		}
		if (nullptr == ps.ins)
		{
			Log(LOG_ERROR,nullptr
				, "fun=SendUserDataImd;msg=ctp miss symbol %s when processing position;key=%s;bid=%s;user_name=%s"
				, symbol.c_str()
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
			);
			continue;
		}
		ps.volume_long = ps.volume_long_his + ps.volume_long_today;
		ps.volume_short = ps.volume_short_his + ps.volume_short_today;
		ps.volume_long_frozen = ps.volume_long_frozen_today + ps.volume_long_frozen_his;
		ps.volume_short_frozen = ps.volume_short_frozen_today + ps.volume_short_frozen_his;
		ps.margin = ps.margin_long + ps.margin_short;
		double last_price = ps.ins->last_price;
		if (!IsValid(last_price))
			last_price = ps.ins->pre_settlement;
		if (IsValid(last_price) && (last_price != ps.last_price || ps.changed))
		{
			ps.last_price = last_price;
			ps.position_profit_long = ps.last_price * ps.volume_long * ps.ins->volume_multiple - ps.position_cost_long;
			ps.position_profit_short = ps.position_cost_short - ps.last_price * ps.volume_short * ps.ins->volume_multiple;
			ps.position_profit = ps.position_profit_long + ps.position_profit_short;
			ps.float_profit_long = ps.last_price * ps.volume_long * ps.ins->volume_multiple - ps.open_cost_long;
			ps.float_profit_short = ps.open_cost_short - ps.last_price * ps.volume_short * ps.ins->volume_multiple;
			ps.float_profit = ps.float_profit_long + ps.float_profit_short;
			if (ps.volume_long > 0)
			{
				ps.open_price_long = ps.open_cost_long / (ps.volume_long * ps.ins->volume_multiple);
				ps.position_price_long = ps.position_cost_long / (ps.volume_long * ps.ins->volume_multiple);
			}
			if (ps.volume_short > 0)
			{
				ps.open_price_short = ps.open_cost_short / (ps.volume_short * ps.ins->volume_multiple);
				ps.position_price_short = ps.position_cost_short / (ps.volume_short * ps.ins->volume_multiple);
			}
			ps.changed = true;
			m_something_changed = true;
		}
		if (IsValid(ps.position_profit))
			total_position_profit += ps.position_profit;
		if (IsValid(ps.float_profit))
			total_float_profit += ps.float_profit;
	}

	//重算资金账户
	if (m_something_changed)
	{
		Account& acc = GetAccount("CNY");
		double dv = total_position_profit - acc.position_profit;
		double po_ori = 0;
		double po_curr = 0;
		double av_diff = 0;

		switch (m_Algorithm_Type)
		{
		case THOST_FTDC_AG_All:
			po_ori = acc.position_profit;
			po_curr = total_position_profit;
			break;
		case THOST_FTDC_AG_OnlyLost:
			if (acc.position_profit < 0)
			{
				po_ori = acc.position_profit;
			}
			if (total_position_profit < 0)
			{
				po_curr = total_position_profit;
			}
			break;
		case THOST_FTDC_AG_OnlyGain:
			if (acc.position_profit > 0)
			{
				po_ori = acc.position_profit;
			}
			if (total_position_profit > 0)
			{
				po_curr = total_position_profit;
			}
			break;
		case THOST_FTDC_AG_None:
			po_ori = 0;
			po_curr = 0;
			break;
		default:
			break;
		}

		av_diff = po_curr - po_ori;
		acc.position_profit = total_position_profit;
		acc.float_profit = total_float_profit;
		acc.available += av_diff;
		acc.balance += dv;
		if (IsValid(acc.available) && IsValid(acc.balance) && !IsZero(acc.balance))
			acc.risk_ratio = 1.0 - acc.available / acc.balance;
		else
			acc.risk_ratio = NAN;
		acc.changed = true;
	}

	//构建数据包		
	SerializerTradeBase nss;
	nss.dump_all = true;
	rapidjson::Pointer("/aid").Set(*nss.m_doc, "rtn_data");
	rapidjson::Value node_data;
	nss.FromVar(m_data, &node_data);
	rapidjson::Value node_user_id;
	node_user_id.SetString(_req_login.user_name, nss.m_doc->GetAllocator());
	rapidjson::Value node_user;
	node_user.SetObject();
	node_user.AddMember(node_user_id, node_data, nss.m_doc->GetAllocator());
	rapidjson::Pointer("/data/0/trade").Set(*nss.m_doc, node_user);

	std::string json_str;
	nss.ToString(&json_str);

	//发送	
	std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
	_ios.post(boost::bind(&traderctp::SendMsg, this, connectId, msg_ptr));
}

void traderctp::AfterLogin()
{
	if (g_config.auto_confirm_settlement)
	{
		if (0 == m_confirm_settlement_status.load())
		{
			m_confirm_settlement_status.store(1);
		}
		ReqConfirmSettlement();
	}
	else if (m_settlement_info.empty())
	{
		ReqQrySettlementInfoConfirm();
	}
	m_req_position_id++;
	m_req_account_id++;
	m_need_query_bank.store(true);
	m_need_query_register.store(true);
	m_need_query_broker_trading_params.store(true);
}

#pragma endregion

#pragma region systemlogic

void traderctp::Start()
{
	try
	{
		_out_mq_ptr = std::shared_ptr<boost::interprocess::message_queue>
			(new boost::interprocess::message_queue(boost::interprocess::open_only
				, _out_mq_name.c_str()));

		_in_mq_ptr = std::shared_ptr<boost::interprocess::message_queue>
			(new boost::interprocess::message_queue(boost::interprocess::open_only
				, _in_mq_name.c_str()));
	}
	catch (const std::exception& ex)
	{
		Log(LOG_ERROR,nullptr
			, "fun=Start;msg=open message_queue;key=%s;errmsg=%s"
			,_key.c_str()
			,ex.what());
	}

	try
	{

		m_run_receive_msg.store(true);
		_thread_ptr = boost::make_shared<boost::thread>(
			boost::bind(&traderctp::ReceiveMsg,this,_key));
	}
	catch (const std::exception& ex)
	{
		Log(LOG_ERROR, nullptr
			, "fun=Start;msg=trade ctp start ReceiveMsg thread fail;key=%s;errmsg=%s"
			, _key.c_str()
			, ex.what());
	}
}

void traderctp::ReceiveMsg(const std::string& key)
{
	std::string strKey = key;
	char buf[MAX_MSG_LENTH + 1];
	unsigned int priority = 0;
	boost::interprocess::message_queue::size_type recvd_size = 0;
	while (m_run_receive_msg.load())
	{
		try
		{
			memset(buf, 0, sizeof(buf));
			boost::posix_time::ptime tm = boost::get_system_time()
				+ boost::posix_time::milliseconds(100);
			bool flag = _in_mq_ptr->timed_receive(buf, sizeof(buf), recvd_size, priority, tm);
			if (!m_run_receive_msg.load())
			{
				break;
			}
			if (!flag)
			{
				_ios.post(boost::bind(&traderctp::OnIdle, this));
				continue;
			}
			std::string line = buf;
			if (line.empty())
			{
				continue;
			}

			int connId = -1;
			std::string msg = "";
			int nPos = line.find_first_of('|');
			if ((nPos <= 0) || (nPos + 1 >= line.length()))
			{
				Log(LOG_WARNING,nullptr
					,"fun=ReceiveMsg;msg=traderctp ReceiveMsg is invalid!;key=%s;msgcontent=%s"
					,strKey.c_str()
					,line.c_str());
				continue;
			}
			else
			{
				std::string strId = line.substr(0, nPos);
				connId = atoi(strId.c_str());
				msg = line.substr(nPos + 1);
				std::shared_ptr<std::string> msg_ptr(new std::string(msg));
				_ios.post(boost::bind(&traderctp::ProcessInMsg
					, this, connId, msg_ptr));
			}
		}
		catch (const std::exception& ex)
		{
			Log(LOG_ERROR,nullptr
				, "fun=ReceiveMsg;msg=traderctp ReceiveMsg exception;key=%s;errmsg=%s"
				, strKey.c_str()
				, ex.what());
			break;
		}
	}
}

void traderctp::Stop()
{
	if (nullptr != _thread_ptr)
	{
		m_run_receive_msg.store(false);
		_thread_ptr->join();
	}

	StopTdApi();
}

bool traderctp::IsConnectionLogin(int nId)
{
	bool flag = false;
	for (auto connId : m_logined_connIds)
	{
		if (connId == nId)
		{
			flag = true;
			break;
		}
	}
	return flag;
}

std::string traderctp::GetConnectionStr()
{
	std::string str = "";
	if (m_logined_connIds.empty())
	{
		return str;
	}

	std::stringstream ss;
	for (int i = 0; i < m_logined_connIds.size(); ++i)
	{
		if ((i + 1) == m_logined_connIds.size())
		{
			ss << m_logined_connIds[i];
		}
		else
		{
			ss << m_logined_connIds[i] << "|";
		}
	}
	str = ss.str();
	return str;
}

void traderctp::CloseConnection(int nId)
{
	Log(LOG_INFO,nullptr
		, "fun=CloseConnection;msg=traderctp CloseConnection;key=%s;bid=%s;user_name=%s;connid=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, nId);

	for (std::vector<int>::iterator it = m_logined_connIds.begin();
		it != m_logined_connIds.end(); it++)
	{
		if (*it == nId)
		{
			m_logined_connIds.erase(it);
			break;
		}
	}	
}

void traderctp::ProcessInMsg(int connId, std::shared_ptr<std::string> msg_ptr)
{
	if (nullptr == msg_ptr)
	{
		return;
	}
	std::string& msg = *msg_ptr;
	//一个特殊的消息
	if (msg == CLOSE_CONNECTION_MSG)
	{
		CloseConnection(connId);
		return;
	}

	SerializerTradeBase ss;
	if (!ss.FromString(msg.c_str()))
	{
		Log(LOG_WARNING,nullptr
			, "fun=ProcessInMsg;msg=traderctp parse json fail;key=%s;bid=%s;user_name=%s;msgcontent=%s;connid=%d"			
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, msg.c_str()
			, connId);
		return;
	}

	ReqLogin req;
	ss.ToVar(req);
	if (req.aid == "req_login")
	{
		ProcessReqLogIn(connId, req);
	}
	else if (req.aid == "change_password")
	{
		if (nullptr == m_pTdApi)
		{
			Log(LOG_ERROR,nullptr
				, "fun=ProcessInMsg;msg=trade ctp receive change_password msg before receive login msg;key=%s;bid=%s;user_name=%s;connid=%d"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, connId);
			return;
		}

		if ((!m_b_login.load()) && (m_loging_connectId != connId))
		{
			Log(LOG_ERROR, nullptr
				, "fun=ProcessInMsg;msg=trade ctp receive change_password msg from a diffrent connection before login suceess;key=%s;bid=%s;user_name=%s;connid=%d"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, connId);
			return;
		}

		m_loging_connectId = connId;

		SerializerCtp ssCtp;
		if (!ssCtp.FromString(msg.c_str()))
		{
			return;
		}

		rapidjson::Value* dt = rapidjson::Pointer("/aid").Get(*(ssCtp.m_doc));
		if (!dt || !dt->IsString())
		{
			return;
		}

		std::string aid = dt->GetString();
		if (aid == "change_password")
		{
			CThostFtdcUserPasswordUpdateField f;
			memset(&f, 0, sizeof(f));
			ssCtp.ToVar(f);
			OnClientReqChangePassword(f);
		}
	}
	else
	{
		if (!m_b_login)
		{
			Log(LOG_WARNING,msg.c_str()
				,"fun=ProcessInMsg;msg=trade ctp receive other msg before login;key=%s;bid=%s;user_name=%s;connid=%d"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, connId);
			return;
		}
			   
		if (!IsConnectionLogin(connId)
			&&(connId!=0))
		{
			Log(LOG_WARNING,msg.c_str()
				, "fun=ProcessInMsg;msg=trade ctp receive other msg which from not login connecion;key=%s;bid=%s;user_name=%s;connid=%d"				
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, connId);
			return;
		}

		SerializerCtp ssCtp;
		if (!ssCtp.FromString(msg.c_str()))
		{
			return;
		}

		rapidjson::Value* dt = rapidjson::Pointer("/aid").Get(*(ssCtp.m_doc));
		if (!dt || !dt->IsString())
		{
			return;
		}

		std::string aid = dt->GetString();
		if (aid == "peek_message")
		{
			OnClientPeekMessage();
		}
		else if (aid == "req_reconnect_trade")
		{
			SerializerConditionOrderData ns;
			if (!ns.FromString(msg.c_str()))
			{
				return;
			}
			req_reconnect_trade_instance req_reconnect_trade;
			ns.ToVar(req_reconnect_trade);
			for (auto id : req_reconnect_trade.connIds)
			{
				m_logined_connIds.push_back(id);
			}
		}
		else if (aid == "insert_order")
		{
			if (nullptr == m_pTdApi)
			{
				OutputNotifyAllSycn(0,u8"当前时间不支持下单! ","WARNING");
				return;
			}
			CtpActionInsertOrder d;
			ssCtp.ToVar(d);
			OnClientReqInsertOrder(d);
		}
		else if (aid == "cancel_order")
		{
			if (nullptr == m_pTdApi)
			{
				OutputNotifyAllSycn(0, u8"当前时间不支持撤单! ", "WARNING");
				return;
			}
			CtpActionCancelOrder d;
			ssCtp.ToVar(d);
			OnClientReqCancelOrder(d);
		}
		else if (aid == "req_transfer")
		{
			if (nullptr == m_pTdApi)
			{
				OutputNotifyAllSycn(0, u8"当前时间不支持转账! ", "WARNING");
				return;
			}
			CThostFtdcReqTransferField f;
			memset(&f, 0, sizeof(f));
			ssCtp.ToVar(f);
			OnClientReqTransfer(f);
		}
		else if (aid == "confirm_settlement")
		{
			if (nullptr == m_pTdApi)
			{
				OutputNotifyAllSycn(0, u8"当前时间不支持确认结算单! ", "WARNING");
				return;
			}

			if (0 == m_confirm_settlement_status.load())
			{
				m_confirm_settlement_status.store(1);
			}
			ReqConfirmSettlement();
		}
		else if (aid == "qry_settlement_info")
		{
			if (nullptr == m_pTdApi)
			{
				OutputNotifyAllSycn(0, u8"当前时间不支持查询历史结算单! ", "WARNING");
				return;
			}

			qry_settlement_info qrySettlementInfo;
			ss.ToVar(qrySettlementInfo);
			OnClientReqQrySettlementInfo(qrySettlementInfo);
		}
		else if (aid == "insert_condition_order")
		{
			m_condition_order_manager.InsertConditionOrder(msg);
		}
		else if (aid == "cancel_condition_order")
		{
			m_condition_order_manager.CancelConditionOrder(msg);
		}
		else if (aid == "pause_condition_order")
		{
			m_condition_order_manager.PauseConditionOrder(msg);
		}
		else if (aid == "resume_condition_order")
		{
			m_condition_order_manager.ResumeConditionOrder(msg);
		}
		else if (aid == "qry_his_condition_order")
		{
			m_condition_order_manager.QryHisConditionOrder(msg);
		}
		else if (aid == "req_ccos_status")
		{
			Log(LOG_INFO, msg.c_str()
				, "fun=ProcessInMsg;msg=trade ctp receive ccos msg;key=%s;bid=%s;user_name=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str());			
			m_condition_order_manager.ChangeCOSStatus(msg);
		}
		else if (aid == "req_start_ctp")
		{
			Log(LOG_INFO, msg.c_str()
				, "fun=ProcessInMsg;msg=trade ctp receive req_start_ctp msg;key=%s;bid=%s;user_name=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str());
			OnReqStartCTP(msg);
		}
		else if (aid == "req_stop_ctp")
		{
			Log(LOG_INFO, msg.c_str()
			, "fun=ProcessInMsg;msg=trade ctp receive req_stop_ctp msg;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
			OnReqStopCTP(msg);
		}
	}
}

void traderctp::ClearOldData()
{
	if (m_need_save_file.load())
	{
		SaveToFile();
	}

	StopTdApi();
	m_b_login.store(false);
	_logIn_status = 0;
	m_try_req_authenticate_times = 0;
	m_try_req_login_times = 0;
	m_ordermap_local_remote.clear();
	m_ordermap_remote_local.clear();

	m_data.m_accounts.clear();
	m_data.m_banks.clear();
	m_data.m_orders.clear();
	m_data.m_positions.clear();
	m_data.m_trades.clear();
	m_data.m_transfers.clear();
	m_data.m_trade_more_data = false;

	m_banks.clear();

	m_settlement_info = "";

	m_notify_seq = 0;
	m_data_seq = 0;
	_requestID.store(0);

	m_trading_day = "";
	m_front_id = 0;
	m_session_id = 0;
	m_order_ref = 0;

	m_req_login_dt = 0;
	m_next_qry_dt = 0;
	m_next_send_dt = 0;

	m_need_query_settlement.store(false);
	m_confirm_settlement_status.store(0);

	m_req_account_id.store(0);
	m_rsp_account_id.store(0);

	m_req_position_id.store(0);
	m_rsp_position_id.store(0);
	m_position_ready.store(false);

	m_need_query_bank.store(false);
	m_need_query_register.store(false);

	m_something_changed = false;
	m_peeking_message = false;

	m_need_save_file.store(false);

	m_need_query_broker_trading_params.store(false);
	m_Algorithm_Type = THOST_FTDC_AG_None;

	m_is_qry_his_settlement_info.store(false);
	m_his_settlement_info = "";
	m_qry_his_settlement_info_trading_days.clear();

	m_position_inited.store(false);
}

void traderctp::OnReqStartCTP(const std::string& msg)
{
	Log(LOG_INFO, msg.c_str()
		, "fun=OnReqStartCTP;msg=req start ctp;key=%s;bid=%s;user_name=%s"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str());

	SerializerConditionOrderData nss;
	if (!nss.FromString(msg.c_str()))
	{
		Log(LOG_WARNING, nullptr
			, "fun=OnReqStartCTP;msg=traderctp parse json fail;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
		return;
	}
	
	req_start_trade_instance req;
	nss.ToVar(req);
	if (req.aid != "req_start_ctp")
	{
		return;
	}

	//如果CTP已经登录成功
	if (m_b_login.load())
	{
		Log(LOG_INFO, msg.c_str()
			, "fun=OnReqStartCTP;msg=has login success instance req start ctp;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());

		ClearOldData();
		
		ReqLogin reqLogin;
		reqLogin.aid = "req_login";
		reqLogin.bid = req.bid;
		reqLogin.user_name = req.user_name;
		reqLogin.password = req.password;

		ProcessReqLogIn(0, reqLogin);
	}
	else
	{
		Log(LOG_INFO, msg.c_str()
			, "fun=OnReqStartCTP;msg=not login instance req start ctp;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());

		ReqLogin reqLogin;
		reqLogin.aid = "req_login";
		reqLogin.bid = req.bid;
		reqLogin.user_name = req.user_name;
		reqLogin.password = req.password;

		ProcessReqLogIn(0,reqLogin);
	}
}

void traderctp::OnReqStopCTP(const std::string& msg)
{
	Log(LOG_INFO, msg.c_str()
		, "fun=OnReqStopCTP;msg=req stop ctp;key=%s;bid=%s;user_name=%s"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str());

	SerializerConditionOrderData nss;
	if (!nss.FromString(msg.c_str()))
	{
		Log(LOG_WARNING, nullptr
			, "fun=OnReqStopCTP;msg=traderctp parse json fail;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
		return;
	}

	req_start_trade_instance req;
	nss.ToVar(req);
	if (req.aid != "req_stop_ctp")
	{
		return;
	}

	//如果CTP已经登录成功
	if (m_b_login.load())
	{
		Log(LOG_INFO, msg.c_str()
			, "fun=OnReqStopCTP;msg=has login success instance req stop ctp;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());

		if (m_need_save_file.load())
		{
			SaveToFile();
		}

		StopTdApi();
	}
	else
	{
		Log(LOG_INFO, msg.c_str()
			, "fun=OnReqStopCTP;msg=not login instance req stop ctp;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());

		StopTdApi();
	}
}

void traderctp::OnClientReqQrySettlementInfo(const qry_settlement_info
	& qrySettlementInfo)
{
	m_qry_his_settlement_info_trading_days.push_back(qrySettlementInfo.trading_day);
}

void traderctp::ProcessReqLogIn(int connId, ReqLogin& req)
{
	Log(LOG_INFO,nullptr
		, "fun=ProcessReqLogIn;msg=traderctp ProcessReqLogIn;key=%s;bid=%s;user_name=%s;client_ip=%s;client_port=%d;client_app_id=%s;client_system_info_length=%d;front=%s;broker_id=%s;connId=%d"
		, _key.c_str()
		, req.bid.c_str()
		, req.user_name.c_str()
		, req.client_ip.c_str()
		, req.client_port
		, req.client_app_id.c_str()	
		, req.client_system_info.length()
		, req.front.c_str()
		, req.broker_id.c_str()
		, connId);

	//如果CTP已经登录成功
	if (m_b_login.load())
	{
		//判断是否重复登录
		bool flag = false;
		for (auto id : m_logined_connIds)
		{
			if (id == connId)
			{
				flag = true;
				break;
			}
		}
		
		if (flag)
		{
			OutputNotifySycn(connId, 0, u8"重复发送登录请求!");
			return;
		}

		//简单比较登陆凭证,判断是否能否成功登录
		if ((_req_login.bid == req.bid)
			&& (_req_login.user_name == req.user_name)
			&& (_req_login.password == req.password))
		{
			if (0 != connId)
			{
				//加入登录客户端列表
				m_logined_connIds.push_back(connId);
				OutputNotifySycn(connId, 0, u8"登录成功");

				char json_str[1024];
				sprintf(json_str, (u8"{"\
					"\"aid\": \"rtn_data\","\
					"\"data\" : [{\"trade\":{\"%s\":{\"session\":{"\
					"\"user_id\" : \"%s\","\
					"\"trading_day\" : \"%s\""
					"}}}}]}")
					, _req_login.user_name.c_str()
					, _req_login.user_name.c_str()
					, m_trading_day.c_str());
				std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
				_ios.post(boost::bind(&traderctp::SendMsg, this, connId, msg_ptr));

				//发送用户数据
				SendUserDataImd(connId);

				m_condition_order_manager.SendAllConditionOrderDataImd(connId);

				//重发结算结果确认信息
				ReSendSettlementInfo(connId);
			}
		}
		else
		{
			if (0 != connId)
			{
				OutputNotifySycn(connId, 0, u8"用户登录失败!");
			}			
		}
	}
	else
	{
		_req_login = req;
		auto it = g_config.brokers.find(_req_login.bid);
		_req_login.broker = it->second;

		//为了支持次席而添加的功能
		if ((!_req_login.broker_id.empty()) &&
			(!_req_login.front.empty()))
		{
			Log(LOG_INFO,nullptr
				, "fun=ProcessReqLogIn;msg=ctp login from custom front and broker_id;key=%s;bid=%s;user_name=%s;broker_id=%s;front=%s"
				, _key.c_str()
				, req.bid.c_str()
				, req.user_name.c_str()				
				, req.broker_id.c_str()
				, req.front.c_str());

			_req_login.broker.ctp_broker_id = _req_login.broker_id;
			_req_login.broker.trading_fronts.clear();
			_req_login.broker.trading_fronts.push_back(_req_login.front);
		}

		if (!g_config.user_file_path.empty())
		{
			m_user_file_path = g_config.user_file_path + "/" + _req_login.bid;
		}

		m_data.user_id = _req_login.user_name;
		LoadFromFile();
		m_loging_connectId = connId;
		if (nullptr != m_pTdApi)
		{
			StopTdApi();
		}
		InitTdApi();
		int login_status = WaitLogIn();
		if (0 == login_status)
		{
			m_b_login.store(false);
			StopTdApi();
		}
		else if (1 == login_status)
		{
			m_b_login.store(false);
		}
		else if (2 == login_status)
		{
			m_b_login.store(true);
		}

		//如果登录成功
		if (m_b_login.load())
		{
			//加入登录客户端列表
			if (connId != 0)
			{
				m_logined_connIds.push_back(connId);

				char json_str[1024];
				sprintf(json_str, (u8"{"\
					"\"aid\": \"rtn_data\","\
					"\"data\" : [{\"trade\":{\"%s\":{\"session\":{"\
					"\"user_id\" : \"%s\","\
					"\"trading_day\" : \"%s\""
					"}}}}]}")
					, _req_login.user_name.c_str()
					, _req_login.user_name.c_str()
					, m_trading_day.c_str());
				std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
				_ios.post(boost::bind(&traderctp::SendMsg, this, connId, msg_ptr));
			}
			
			//加载条件单数据
			m_condition_order_manager.Load(_req_login.bid,
				_req_login.user_name,
				_req_login.password,
				m_trading_day);
		}
		else
		{
			if (connId != 0)
			{
				m_loging_connectId = connId;
				OutputNotifySycn(connId, 0, u8"用户登录失败!");
			}			
		}
	}
}

int traderctp::WaitLogIn()
{
	boost::unique_lock<boost::mutex> lock(_logInmutex);
	_logIn_status = 0;
	m_pTdApi->Init();
	bool notify = _logInCondition.timed_wait(lock, boost::posix_time::seconds(15));
	if (0 == _logIn_status)
	{
		if (!notify)
		{
			Log(LOG_WARNING,nullptr
				, "fun=WaitLogIn;msg=CTP login timeout,trading fronts is closed or trading fronts config is error;key=%s;bid=%s;user_name=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str());
		}
	}
	return _logIn_status;
}

void traderctp::InitTdApi()
{
	m_try_req_authenticate_times = 0;
	m_try_req_login_times = 0;
	std::string flow_file_name = GenerateUniqFileName();
	m_pTdApi = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_file_name.c_str());
	m_pTdApi->RegisterSpi(this);
	m_pTdApi->SubscribePrivateTopic(THOST_TERT_RESUME);
	m_pTdApi->SubscribePublicTopic(THOST_TERT_RESUME);
	m_broker_id = _req_login.broker.ctp_broker_id;

	if (_req_login.broker.is_fens)
	{
		Log(LOG_INFO, nullptr
			, "fun=InitTdApi;msg=fens address is used;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());

		CThostFtdcFensUserInfoField field;
		memset(&field, 0, sizeof(field));
		strcpy_x(field.BrokerID, _req_login.broker.ctp_broker_id.c_str());
		strcpy_x(field.UserID, _req_login.user_name.c_str());
		field.LoginMode = THOST_FTDC_LM_Trade;
		m_pTdApi->RegisterFensUserInfo(&field);

		for (auto it = _req_login.broker.trading_fronts.begin()
			; it != _req_login.broker.trading_fronts.end(); ++it)
		{
			std::string& f = *it;
			m_pTdApi->RegisterNameServer((char*)(f.c_str()));
		}
	}
	else
	{
		for (auto it = _req_login.broker.trading_fronts.begin()
			; it != _req_login.broker.trading_fronts.end(); ++it)
		{
			std::string& f = *it;
			m_pTdApi->RegisterFront((char*)(f.c_str()));
		}
	}
}

void traderctp::StopTdApi()
{
	if (nullptr != m_pTdApi)
	{
		Log(LOG_INFO, nullptr
			, "fun=StopTdApi;msg=ctp OnFinish;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
		m_pTdApi->RegisterSpi(NULL);
		m_pTdApi->Release();
		m_pTdApi = NULL;
	}
}

void traderctp::SendMsg(int connId, std::shared_ptr<std::string> msg_ptr)
{
	if (nullptr == msg_ptr)
	{
		return;
	}

	if (nullptr == _out_mq_ptr)
	{
		return;
	}

	std::string& msg = *msg_ptr;
	std::stringstream ss;
	ss << connId << "#";
	msg = ss.str() + msg;

	size_t totalLength = msg.length();
	if (totalLength > MAX_MSG_LENTH)
	{
		try
		{
			_out_mq_ptr->send(BEGIN_OF_PACKAGE.c_str(), BEGIN_OF_PACKAGE.length(), 0);
			const char* buffer = msg.c_str();
			size_t start_pos = 0;
			while (true)
			{
				if ((start_pos + MAX_MSG_LENTH) < totalLength)
				{
					_out_mq_ptr->send(buffer + start_pos, MAX_MSG_LENTH, 0);
				}
				else
				{
					_out_mq_ptr->send(buffer + start_pos, totalLength - start_pos, 0);
					break;
				}
				start_pos += MAX_MSG_LENTH;
			}
			_out_mq_ptr->send(END_OF_PACKAGE.c_str(), END_OF_PACKAGE.length(), 0);
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR,nullptr
				, "fun=SendMsg;msg=SendMsg exception,%s;length=%d;key=%s;bid=%s;user_name=%s"
				, ex.what()
				, msg.length()
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str());
		}
	}
	else
	{
		try
		{
			_out_mq_ptr->send(msg.c_str(), totalLength, 0);
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR,nullptr
				, "fun=SendMsg;msg=SendMsg exception,%s;length=%d;key=%s;bid=%s;user_name=%s"
				, ex.what()	
				, totalLength
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str());
		}
	}
}

void traderctp::SendMsgAll(std::shared_ptr<std::string> conn_str_ptr, std::shared_ptr<std::string> msg_ptr)
{
	if (nullptr == msg_ptr)
	{
		return;
	}

	if (nullptr == conn_str_ptr)
	{
		return;
	}

	if (nullptr == _out_mq_ptr)
	{
		return;
	}

	std::string& msg = *msg_ptr;
	std::string& conn_str = *conn_str_ptr;
	msg = conn_str + "#" + msg;

	size_t totalLength = msg.length();
	if (totalLength > MAX_MSG_LENTH)
	{
		try
		{
			_out_mq_ptr->send(BEGIN_OF_PACKAGE.c_str(), BEGIN_OF_PACKAGE.length(), 0);
			const char* buffer = msg.c_str();
			size_t start_pos = 0;
			while (true)
			{
				if ((start_pos + MAX_MSG_LENTH) < totalLength)
				{
					_out_mq_ptr->send(buffer + start_pos, MAX_MSG_LENTH, 0);
				}
				else
				{
					_out_mq_ptr->send(buffer + start_pos, totalLength - start_pos, 0);
					break;
				}
				start_pos += MAX_MSG_LENTH;
			}
			_out_mq_ptr->send(END_OF_PACKAGE.c_str(), END_OF_PACKAGE.length(), 0);
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR,nullptr
				, "fun=SendMsgAll;errmsg=%s;length=%d;key=%s;bid=%s;user_name=%s"
				, ex.what()
				, msg.length()
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str());
		}
	}
	else
	{
		try
		{
			_out_mq_ptr->send(msg.c_str(), totalLength, 0);
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR,nullptr
				, "fun=SendMsgAll;errmsg=%s;length=%d;key=%s;bid=%s;user_name=%s"
				, ex.what()
				, totalLength
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str());
		}
	}
}

void traderctp::OutputNotifyAsych(int connId, long notify_code, const std::string& notify_msg
	, const char* level, const char* type)
{
	//构建数据包
	SerializerTradeBase nss;
	rapidjson::Pointer("/aid").Set(*nss.m_doc, "rtn_data");
	rapidjson::Value node_message;
	node_message.SetObject();
	node_message.AddMember("type", rapidjson::Value(type, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("level", rapidjson::Value(level, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("code", notify_code, nss.m_doc->GetAllocator());
	node_message.AddMember("content", rapidjson::Value(notify_msg.c_str(), nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	rapidjson::Pointer("/data/0/notify/N" + std::to_string(m_notify_seq++)).Set(*nss.m_doc, node_message);
	std::string json_str;
	nss.ToString(&json_str);
	std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
	_ios.post(boost::bind(&traderctp::SendMsg, this, connId, msg_ptr));
}

void traderctp::OutputNotifySycn(int connId, long notify_code
	, const std::string& notify_msg, const char* level
	, const char* type)
{
	//构建数据包
	SerializerTradeBase nss;
	rapidjson::Pointer("/aid").Set(*nss.m_doc, "rtn_data");
	rapidjson::Value node_message;
	node_message.SetObject();
	node_message.AddMember("type", rapidjson::Value(type, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("level", rapidjson::Value(level, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("code", notify_code, nss.m_doc->GetAllocator());
	node_message.AddMember("content", rapidjson::Value(notify_msg.c_str(), nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	rapidjson::Pointer("/data/0/notify/N" + std::to_string(m_notify_seq++)).Set(*nss.m_doc, node_message);
	std::string json_str;
	nss.ToString(&json_str);
	std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
	_ios.post(boost::bind(&traderctp::SendMsg, this, connId, msg_ptr));
}

void traderctp::OutputNotifyAllAsych(long notify_code
	, const std::string& ret_msg, const char* level
	, const char* type)
{
	//构建数据包
	SerializerTradeBase nss;
	rapidjson::Pointer("/aid").Set(*nss.m_doc, "rtn_data");
	rapidjson::Value node_message;
	node_message.SetObject();
	node_message.AddMember("type", rapidjson::Value(type, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("level", rapidjson::Value(level, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("code", notify_code, nss.m_doc->GetAllocator());
	node_message.AddMember("content", rapidjson::Value(ret_msg.c_str(), nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	rapidjson::Pointer("/data/0/notify/N" + std::to_string(m_notify_seq++)).Set(*nss.m_doc, node_message);
	std::string json_str;
	nss.ToString(&json_str);
	std::string str = GetConnectionStr();
	if (!str.empty())
	{
		std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
		std::shared_ptr<std::string> conn_ptr(new std::string(str));
		_ios.post(boost::bind(&traderctp::SendMsgAll, this, conn_ptr, msg_ptr));
	}
}

void traderctp::OutputNotifyAllSycn(long notify_code
	, const std::string& ret_msg, const char* level
	, const char* type)
{
	//构建数据包
	SerializerTradeBase nss;
	rapidjson::Pointer("/aid").Set(*nss.m_doc, "rtn_data");
	rapidjson::Value node_message;
	node_message.SetObject();
	node_message.AddMember("type", rapidjson::Value(type, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("level", rapidjson::Value(level, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	node_message.AddMember("code", notify_code, nss.m_doc->GetAllocator());
	node_message.AddMember("content", rapidjson::Value(ret_msg.c_str(), nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
	rapidjson::Pointer("/data/0/notify/N" + std::to_string(m_notify_seq++)).Set(*nss.m_doc, node_message);
	std::string json_str;
	nss.ToString(&json_str);
	std::string str = GetConnectionStr();
	if (!str.empty())
	{
		std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
		std::shared_ptr<std::string> conn_ptr(new std::string(str));
		_ios.post(boost::bind(&traderctp::SendMsgAll, this, conn_ptr, msg_ptr));
	}
	else
	{
		Log(LOG_INFO, nullptr
			, "fun=OutputNotifyAllSycn;key=%s;bid=%s;user_name=%s;msg=GetConnectionStr is empty"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
	}
}

#pragma endregion

#pragma region client_request

void traderctp::OnClientReqChangePassword(CThostFtdcUserPasswordUpdateField f)
{
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	int r = m_pTdApi->ReqUserPasswordUpdate(&f, 0);
	if (0 != r)
	{
		Log(LOG_INFO, nullptr
			, "fun=OnClientReqChangePassword;key=%s;bid=%s;user_name=%s;ret=%d"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, r);
		OutputNotifyAllSycn(1, u8"修改密码请求发送失败!", "WARNING");
	}	
}

void traderctp::OnClientReqTransfer(CThostFtdcReqTransferField f)
{
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	strcpy_x(f.AccountID, _req_login.user_name.c_str());
	strcpy_x(f.BankBranchID, "0000");
	f.SecuPwdFlag = THOST_FTDC_BPWDF_BlankCheck;	// 核对密码
	f.BankPwdFlag = THOST_FTDC_BPWDF_NoCheck;	// 核对密码
	f.VerifyCertNoFlag = THOST_FTDC_YNI_No;

	if (f.TradeAmount >= 0)
	{
		strcpy_x(f.TradeCode, "202001");
		int nRequestID = _requestID++;
		int r = m_pTdApi->ReqFromBankToFutureByFuture(&f,nRequestID);
		if (0 != r)
		{
			Log(LOG_INFO, nullptr
				, "fun=OnClientReqTransfer;key=%s;bid=%s;user_name=%s;TradeAmount=%f;ret=%d;nRequestID=%d"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, f.TradeAmount
				, r
				, nRequestID);
			OutputNotifyAllSycn(1, u8"银期转账请求发送失败!", "WARNING");
		}		
		m_req_transfer_list.push_back(nRequestID);		
	}
	else
	{
		strcpy_x(f.TradeCode, "202002");
		f.TradeAmount = -f.TradeAmount;
		int nRequestID = _requestID++;
		int r = m_pTdApi->ReqFromFutureToBankByFuture(&f,nRequestID);
		if (0 != r)
		{
			Log(LOG_INFO, nullptr
				, "fun=OnClientReqTransfer;key=%s;bid=%s;user_name=%s;TradeAmount=%f;ret=%d;nRequestID=%d"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, f.TradeAmount
				, r
				, nRequestID);
			OutputNotifyAllSycn(1, u8"银期转账请求发送失败!", "WARNING");
		}		
		m_req_transfer_list.push_back(nRequestID);		
	}
}

void traderctp::OnClientReqCancelOrder(CtpActionCancelOrder d)
{
	if (d.local_key.user_id.substr(0, _req_login.user_name.size()) != _req_login.user_name)
	{
		OutputNotifyAllSycn(1, u8"撤单user_id错误,不能撤单", "WARNING");
		return;
	}

	RemoteOrderKey rkey;
	if (!OrderIdLocalToRemote(d.local_key, &rkey))
	{
		OutputNotifyAllSycn(1, u8"撤单指定的order_id不存在,不能撤单", "WARNING");
		return;
	}
	strcpy_x(d.f.BrokerID, m_broker_id.c_str());
	strcpy_x(d.f.UserID, _req_login.user_name.c_str());
	strcpy_x(d.f.InvestorID, _req_login.user_name.c_str());
	strcpy_x(d.f.OrderRef, rkey.order_ref.c_str());
	strcpy_x(d.f.ExchangeID, rkey.exchange_id.c_str());
	strcpy_x(d.f.InstrumentID, rkey.instrument_id.c_str());
	d.f.SessionID = rkey.session_id;
	d.f.FrontID = rkey.front_id;
	d.f.ActionFlag = THOST_FTDC_AF_Delete;
	d.f.LimitPrice = 0;
	d.f.VolumeChange = 0;
	{
		m_cancel_order_set.insert(d.local_key.order_id);
	}

	std::stringstream ss;
	ss << m_front_id << m_session_id << d.f.OrderRef;
	std::string strKey = ss.str();
	m_action_order_map.insert(
		std::map<std::string, std::string>::value_type(strKey, strKey));

	int r = m_pTdApi->ReqOrderAction(&d.f, 0);
	if (0 != r)
	{
		OutputNotifyAllSycn(1, u8"撤单请求发送失败!", "WARNING");
	}
	Log(LOG_INFO, nullptr
		, "fun=OnClientReqCancelOrder;key=%s;bid=%s;user_name=%s;InstrumentID=%s;OrderRef=%s;ret=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, d.f.InstrumentID
		, d.f.OrderRef
		, r);
}

void traderctp::OnClientReqInsertOrder(CtpActionInsertOrder d)
{
	if (d.local_key.user_id.substr(0, _req_login.user_name.size()) != _req_login.user_name)
	{
		OutputNotifyAllSycn(1, u8"报单user_id错误，不能下单", "WARNING");
		return;
	}

	strcpy_x(d.f.BrokerID, m_broker_id.c_str());
	strcpy_x(d.f.UserID, _req_login.user_name.c_str());
	strcpy_x(d.f.InvestorID, _req_login.user_name.c_str());
	RemoteOrderKey rkey;
	rkey.exchange_id = d.f.ExchangeID;
	rkey.instrument_id = d.f.InstrumentID;
	if (OrderIdLocalToRemote(d.local_key, &rkey))
	{
		OutputNotifyAllSycn(1, u8"报单单号重复，不能下单", "WARNING");
		return;
	}

	strcpy_x(d.f.OrderRef, rkey.order_ref.c_str());
	{
		m_insert_order_set.insert(d.f.OrderRef);
	}

	std::stringstream ss;
	ss << m_front_id << m_session_id << d.f.OrderRef;
	std::string strKey = ss.str();
	ServerOrderInfo serverOrder;
	serverOrder.InstrumentId = rkey.instrument_id;
	serverOrder.ExchangeId = rkey.exchange_id;
	serverOrder.VolumeOrigin = d.f.VolumeTotalOriginal;
	serverOrder.VolumeLeft = d.f.VolumeTotalOriginal;
	m_input_order_key_map.insert(std::map<std::string
		, ServerOrderInfo>::value_type(strKey, serverOrder));

	int r = m_pTdApi->ReqOrderInsert(&d.f, 0);
	if (0 != r)
	{
		OutputNotifyAllSycn(1, u8"下单请求发送失败!", "WARNING");
	}
	Log(LOG_INFO, nullptr
		, "fun=OnClientReqInsertOrder;key=%s;orderid=%s;bid=%s;user_name=%s;InstrumentID=%s;OrderRef=%s;ret=%d;OrderPriceType=%c;Direction=%c;CombOffsetFlag=%c;LimitPrice=%f;VolumeTotalOriginal=%d;VolumeCondition=%c;TimeCondition=%c"
		, _key.c_str()
		, d.local_key.order_id.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, d.f.InstrumentID
		, d.f.OrderRef
		, r
		, d.f.OrderPriceType
		, d.f.Direction
		, d.f.CombHedgeFlag[0]
		, d.f.LimitPrice
		, d.f.VolumeTotalOriginal
		, d.f.VolumeCondition
		, d.f.TimeCondition);
	m_need_save_file.store(true);
}

void traderctp::OnClientPeekMessage()
{
	m_peeking_message = true;
	//向客户端发送账户信息
	SendUserData();
}



#pragma endregion

#pragma region ConditionOrderCallBack

void traderctp::SendConditionOrderData(const std::string& msg)
{
	std::string str = GetConnectionStr();
	if (!str.empty())
	{
		std::shared_ptr<std::string> msg_ptr(new std::string(msg));
		std::shared_ptr<std::string> conn_ptr(new std::string(str));
		_ios.post(boost::bind(&traderctp::SendMsgAll, this, conn_ptr, msg_ptr));
	}
}

void traderctp::SendConditionOrderData(int connectId, const std::string& msg)
{
	std::shared_ptr<std::string> msg_ptr(new std::string(msg));
	_ios.post(boost::bind(&traderctp::SendMsg, this, connectId, msg_ptr));
}


void traderctp::OutputNotifyAll(long notify_code, const std::string& ret_msg
	, const char* level	,const char* type)
{
	OutputNotifyAllSycn(notify_code,ret_msg,level,type);
}

void traderctp::CheckTimeConditionOrder()
{
	std::set<std::string>& os = m_condition_order_manager.GetTimeCoSet();
	if (os.empty())
	{
		return;
	}

	long long currentTime = GetLocalEpochMilli();
	m_condition_order_manager.OnCheckTime(currentTime);
}

void traderctp::CheckPriceConditionOrder()
{
	TInstOrderIdListMap& om = m_condition_order_manager.GetPriceCoMap();
	if (om.empty())
	{
		return;
	}

	m_condition_order_manager.OnCheckPrice();
}

bool traderctp::ConditionOrder_Open(const ConditionOrder& order
	, const ContingentOrder& co
	, const Instrument& ins
	,ctp_condition_order_task& task
	, int nOrderIndex)
{	
	CThostFtdcInputOrderField f;
	memset(&f, 0, sizeof(CThostFtdcInputOrderField));
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	strcpy_x(f.InvestorID, _req_login.user_name.c_str());
	strcpy_x(f.ExchangeID, co.exchange_id.c_str());
	strcpy_x(f.InstrumentID, co.instrument_id.c_str());

	//开仓
	f.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	   
	//开多
	if (EOrderDirection::buy == co.direction)
	{
		f.Direction = THOST_FTDC_D_Buy;
	}
	//开空
	else
	{
		f.Direction = THOST_FTDC_D_Sell;
	}

	//数量类型
	if (EVolumeType::num == co.volume_type)
	{
		f.VolumeTotalOriginal = co.volume;
		f.VolumeCondition = THOST_FTDC_VC_AV;
	}
	else
	{
		//开仓时必须指定具体手数
		Log(LOG_WARNING, nullptr
			, "fun=ConditionOrder_Open;msg=has bad volume_type;key=%s;bid=%s;user_name=%s;instrument_id=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, co.instrument_id.c_str());
		return false;
	}

	//价格类型

	//限价
	if (EPriceType::limit == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		f.LimitPrice = co.limit_price;
	}
	//触发价
	else if (EPriceType::contingent == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		bool flag = false;
		for (const ContingentCondition& c : order.condition_list)
		{
			if ((c.contingent_type == EContingentType::price)
				&& (c.exchange_id == co.exchange_id)
				&& (c.instrument_id == co.instrument_id))
			{
				f.LimitPrice = c.contingent_price;
				flag = true;
				break;
			}
		}
		if (!flag)
		{
			//找不到触发价
			Log(LOG_WARNING, nullptr
				, "fun=ConditionOrder_Open;msg=can not find contingent_price;key=%s;bid=%s;user_name=%s;instrument_id=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, co.instrument_id.c_str());
			return false;
		}
	}
	//对价
	else if (EPriceType::consideration == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//开多
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.ask_price1;
		}
		//开空
		else
		{
			f.LimitPrice = ins.bid_price1;
		}
	}
	//市价
	else if (EPriceType::market == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_IOC;
		//开多
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.upper_limit;
		}
		//开空
		else
		{
			f.LimitPrice = ins.lower_limit;
		}
	}
	//超价
	else if (EPriceType::over == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//开多
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.bid_price1 + ins.price_tick;
			if (f.LimitPrice > ins.upper_limit)
			{
				f.LimitPrice = ins.upper_limit;
			}
		}
		//开空
		else
		{
			f.LimitPrice = ins.ask_price1 - ins.price_tick;
			if (f.LimitPrice < ins.lower_limit)
			{
				f.LimitPrice = ins.lower_limit;
			}
		}
	}

	f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
	f.ContingentCondition = THOST_FTDC_CC_Immediately;
	f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

	task.has_order_to_cancel = false;
	task.has_first_orders_to_send = true;
	task.has_second_orders_to_send = false;

	CtpActionInsertOrder actionInsertOrder;
	actionInsertOrder.local_key.user_id = _req_login.user_name;
	actionInsertOrder.local_key.order_id = order.order_id + "_open_" + std::to_string(nOrderIndex);
	actionInsertOrder.f = f;
	task.first_orders_to_send.push_back(actionInsertOrder);
	
	return true;
}

bool traderctp::ConditionOrder_CloseToday(const ConditionOrder& order
	, const ContingentOrder& co
	, const Instrument& ins
	, ctp_condition_order_task& task
	, int nOrderIndex)
{
	bool b_has_td_yd_distinct = (co.exchange_id == "SHFE") || (co.exchange_id == "INE");
	if (!b_has_td_yd_distinct)
	{
		Log(LOG_WARNING, nullptr
			, "fun=ConditionOrder_CloseToday;msg=exchange not support close_today command;key=%s;bid=%s;user_name=%s;instrument_id=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, co.instrument_id.c_str());
		return false;
	}
	
	std::string symbol = co.exchange_id + "." + co.instrument_id;
	
	CThostFtdcInputOrderField f;
	memset(&f, 0, sizeof(CThostFtdcInputOrderField));
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	strcpy_x(f.InvestorID, _req_login.user_name.c_str());
	strcpy_x(f.ExchangeID, co.exchange_id.c_str());
	strcpy_x(f.InstrumentID, co.instrument_id.c_str());

	ENeedCancelOrderType needCancelOrderType = ENeedCancelOrderType::not_need;

	//平今
	f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
	//买平
	if (EOrderDirection::buy == co.direction)
	{
		f.Direction = THOST_FTDC_D_Buy;
		Position& pos = GetPosition(symbol);
		//数量类型
		if (EVolumeType::num == co.volume_type)
		{
			//要平的手数小于等于可平的今仓手数
			if (co.volume <= pos.pos_short_today- pos.volume_short_frozen_today)
			{
				f.VolumeTotalOriginal = co.volume;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			//要平的手数小于等于今仓手数(包括冻结的手数)
			else if (co.volume <= pos.pos_short_today)
			{
				if (order.is_cancel_ori_close_order)
				{
					f.VolumeTotalOriginal = co.volume;
					f.VolumeCondition = THOST_FTDC_VC_AV;
					needCancelOrderType = ENeedCancelOrderType::today_buy;
				}
				else
				{
					Log(LOG_WARNING, nullptr
						, "fun=ConditionOrder_CloseToday;msg=can close short is less than will close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
						, _key.c_str()
						, _req_login.bid.c_str()
						, _req_login.user_name.c_str()
						, co.instrument_id.c_str());
					return false;
				}				
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseToday;msg=can close short is less than will close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
		else if (EVolumeType::close_all == co.volume_type)
		{
			Position& position = GetPosition(symbol);
			//如果可平手数大于零
			if (pos.pos_short_today - pos.volume_short_frozen_today>0)
			{
				f.VolumeTotalOriginal = pos.pos_short_today - pos.volume_short_frozen_today;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseToday;msg=have no need close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
		
	}
	//卖平
	else
	{
		f.Direction = THOST_FTDC_D_Sell;
		Position& pos = GetPosition(symbol);
		//数量类型
		if (EVolumeType::num == co.volume_type)
		{
			//要平的手数小于等于可平的今仓手数
			if (co.volume <= pos.pos_long_today - pos.volume_long_frozen_today)
			{
				f.VolumeTotalOriginal = co.volume;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			//要平的手数小于等于今仓手数(包括冻结的手数)
			else if (co.volume <= pos.pos_long_today)
			{
				if (order.is_cancel_ori_close_order)
				{
					//需要先撤掉所有平今空仓的单子,然后再发送该订单
					f.VolumeTotalOriginal = co.volume;
					f.VolumeCondition = THOST_FTDC_VC_AV;
					needCancelOrderType = ENeedCancelOrderType::today_sell;
				}
				else
				{
					Log(LOG_WARNING, nullptr
						, "fun=ConditionOrder_CloseToday;msg=can close long is less than will close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
						, _key.c_str()
						, _req_login.bid.c_str()
						, _req_login.user_name.c_str()
						, co.instrument_id.c_str());
					return false;
				}
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseToday;msg=can close long is less than will close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
			}
		}
		else if (EVolumeType::close_all == co.volume_type)
		{
			Position& position = GetPosition(symbol);
			//如果可平手数大于零
			if (pos.pos_long_today - pos.volume_long_frozen_today > 0)
			{
				f.VolumeTotalOriginal = pos.pos_long_today - pos.volume_long_frozen_today;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseToday;msg=have no need close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
	}

	//价格类型

	//限价
	if (EPriceType::limit == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		f.LimitPrice = co.limit_price;
	}
	//触发价
	else if (EPriceType::contingent == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;

		bool flag = false;
		for (const ContingentCondition& c : order.condition_list)
		{
			if ((c.contingent_type == EContingentType::price)
				&& (c.exchange_id == co.exchange_id)
				&& (c.instrument_id == co.instrument_id))
			{
				f.LimitPrice = c.contingent_price;
				flag = true;
				break;
			}
		}
		if (!flag)
		{
			//找不到触发价
			Log(LOG_WARNING, nullptr
				, "fun=ConditionOrder_CloseToday;msg=can not find contingent_price;key=%s;bid=%s;user_name=%s;instrument_id=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, co.instrument_id.c_str());
			return false;
		}
	}
	//对价
	else if (EPriceType::consideration == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.ask_price1;
		}
		//卖平
		else
		{
			f.LimitPrice = ins.bid_price1;
		}
	}
	//市价
	else if (EPriceType::market == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_IOC;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.upper_limit;
		}
		//卖平
		else
		{
			f.LimitPrice = ins.lower_limit;
		}
	}
	//超价
	else if (EPriceType::over == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.bid_price1 + ins.price_tick;
			if (f.LimitPrice > ins.upper_limit)
			{
				f.LimitPrice = ins.upper_limit;
			}
		}
		//卖平
		else
		{
			f.LimitPrice = ins.ask_price1 - ins.price_tick;
			if (f.LimitPrice < ins.lower_limit)
			{
				f.LimitPrice = ins.lower_limit;
			}
		}
	}
	
	f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
	f.ContingentCondition = THOST_FTDC_CC_Immediately;
	f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

	task.has_first_orders_to_send = true;
	CtpActionInsertOrder actionInsertOrder;
	actionInsertOrder.local_key.user_id = _req_login.user_name;
	actionInsertOrder.local_key.order_id = order.order_id + "_closetoday_" + std::to_string(nOrderIndex);
	actionInsertOrder.f = f;
	task.first_orders_to_send.push_back(actionInsertOrder);

	task.has_second_orders_to_send = false;
	
	if (ENeedCancelOrderType::today_buy == needCancelOrderType)
	{
		for (auto it : m_data.m_orders)
		{
			const std::string& orderId = it.first;
			const Order& order = it.second;
			if (order.status == kOrderStatusFinished)
			{
				continue;
			}

			if (order.symbol() != symbol)
			{
				continue;
			}

			if ((order.direction == kDirectionBuy) 
				&& (order.offset== kOffsetCloseToday))
			{
				CtpActionCancelOrder cancelOrder;
				cancelOrder.local_key.order_id = orderId;
				cancelOrder.local_key.user_id = _req_login.user_name.c_str();

				CThostFtdcInputOrderActionField f;
				memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
				strcpy_x(f.BrokerID, m_broker_id.c_str());
				strcpy_x(f.UserID, _req_login.user_name.c_str());
				strcpy_x(f.InvestorID, _req_login.user_name.c_str());
				strcpy_x(f.ExchangeID, order.exchange_id.c_str());
				strcpy_x(f.InstrumentID, order.instrument_id.c_str());

				cancelOrder.f = f;

				task.has_order_to_cancel = true;
				task.orders_to_cancel.push_back(cancelOrder);

			}			
		}
	}
	else if (ENeedCancelOrderType::today_sell == needCancelOrderType)
	{
		for (auto it : m_data.m_orders)
		{
			const std::string& orderId = it.first;
			const Order& order = it.second;
			if (order.status == kOrderStatusFinished)
			{
				continue;
			}

			if (order.symbol() != symbol)
			{
				continue;
			}

			if ((order.direction == kDirectionSell)
				&& (order.offset == kOffsetCloseToday))
			{
				CtpActionCancelOrder cancelOrder;
				cancelOrder.local_key.order_id = orderId;
				cancelOrder.local_key.user_id = _req_login.user_name.c_str();

				CThostFtdcInputOrderActionField f;
				memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
				strcpy_x(f.BrokerID, m_broker_id.c_str());
				strcpy_x(f.UserID, _req_login.user_name.c_str());
				strcpy_x(f.InvestorID, _req_login.user_name.c_str());
				strcpy_x(f.ExchangeID, order.exchange_id.c_str());
				strcpy_x(f.InstrumentID, order.instrument_id.c_str());

				cancelOrder.f = f;

				task.has_order_to_cancel = true;
				task.orders_to_cancel.push_back(cancelOrder);
			}
		}
	}
	else
	{
		task.has_order_to_cancel = false;
	}
	
	return true;
}

bool traderctp::ConditionOrder_CloseYesToday(const ConditionOrder& order
	, const ContingentOrder& co
	, const Instrument& ins
	, ctp_condition_order_task& task
	, int nOrderIndex)
{
	std::string symbol = co.exchange_id + "." + co.instrument_id;

	CThostFtdcInputOrderField f;
	memset(&f, 0, sizeof(CThostFtdcInputOrderField));
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	strcpy_x(f.InvestorID, _req_login.user_name.c_str());
	strcpy_x(f.ExchangeID, co.exchange_id.c_str());
	strcpy_x(f.InstrumentID, co.instrument_id.c_str());

	ENeedCancelOrderType needCancelOrderType = ENeedCancelOrderType::not_need;

	//平昨
	f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;

	//买平
	if (EOrderDirection::buy == co.direction)
	{
		f.Direction = THOST_FTDC_D_Buy;
		Position& pos = GetPosition(symbol);

		//数量类型
		if (EVolumeType::num == co.volume_type)
		{
			//要平的手数小于等于可平的昨仓手数
			if (co.volume <= pos.pos_short_his - pos.volume_short_frozen_his)
			{
				f.VolumeTotalOriginal = co.volume;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			//要平的手数小于等于昨仓手数(包括冻结的手数)
			else if (co.volume <= pos.pos_short_his)
			{
				if (order.is_cancel_ori_close_order)
				{
					f.VolumeTotalOriginal = co.volume;
					f.VolumeCondition = THOST_FTDC_VC_AV;
					needCancelOrderType = ENeedCancelOrderType::yestoday_buy;
				}
				else
				{
					Log(LOG_WARNING, nullptr
						, "fun=ConditionOrder_CloseYesToday;msg=can close short is less than will close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
						, _key.c_str()
						, _req_login.bid.c_str()
						, _req_login.user_name.c_str()
						, co.instrument_id.c_str());
					return false;
				}
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseYesToday;msg=can close short is less than will close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
		else if (EVolumeType::close_all == co.volume_type)
		{
			Position& position = GetPosition(symbol);
			//如果可平手数大于零
			if (pos.pos_short_his - pos.volume_short_frozen_his > 0)
			{
				f.VolumeTotalOriginal = pos.pos_short_his - pos.volume_short_frozen_his;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseYesToday;msg=have no need close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
	}
	//卖平
	else
	{
		f.Direction = THOST_FTDC_D_Sell;
		Position& pos = GetPosition(symbol);

		//数量类型
		if (EVolumeType::num == co.volume_type)
		{
			//要平的手数小于等于可平的昨仓手数
			if (co.volume <= pos.pos_long_his - pos.volume_long_frozen_his)
			{
				f.VolumeTotalOriginal = co.volume;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			//要平的手数小于等于昨仓手数(包括冻结的手数)
			else if (co.volume <= pos.pos_long_his)
			{
				if (order.is_cancel_ori_close_order)
				{
					f.VolumeTotalOriginal = co.volume;
					f.VolumeCondition = THOST_FTDC_VC_AV;
					needCancelOrderType = ENeedCancelOrderType::yestoday_sell;
				}
				else
				{
					Log(LOG_WARNING, nullptr
						, "fun=ConditionOrder_CloseYesToday;msg=can close long is less than will close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
						, _key.c_str()
						, _req_login.bid.c_str()
						, _req_login.user_name.c_str()
						, co.instrument_id.c_str());
					return false;
				}
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseYesToday;msg=can close long is less than will close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
			}
		}
		else if (EVolumeType::close_all == co.volume_type)
		{
			Position& position = GetPosition(symbol);
			//如果可平手数大于零
			if (pos.pos_long_his - pos.volume_long_frozen_his > 0)
			{
				f.VolumeTotalOriginal = pos.pos_long_his - pos.volume_long_frozen_his;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_CloseYesToday;msg=have no need close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
	}

	//价格类型

	//限价
	if (EPriceType::limit == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		f.LimitPrice = co.limit_price;
	}
	//触发价
	else if (EPriceType::contingent == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;

		bool flag = false;
		for (const ContingentCondition& c : order.condition_list)
		{
			if ((c.contingent_type == EContingentType::price)
				&& (c.exchange_id == co.exchange_id)
				&& (c.instrument_id == co.instrument_id))
			{
				f.LimitPrice = c.contingent_price;
				flag = true;
				break;
			}
		}
		if (!flag)
		{
			//找不到触发价
			Log(LOG_WARNING, nullptr
				, "fun=ConditionOrder_CloseYesToday;msg=can not find contingent_price;key=%s;bid=%s;user_name=%s;instrument_id=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, co.instrument_id.c_str());
			return false;
		}
	}
	//对价
	else if (EPriceType::consideration == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.ask_price1;
		}
		//卖平
		else
		{
			f.LimitPrice = ins.bid_price1;
		}
	}
	//市价
	else if (EPriceType::market == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_IOC;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.upper_limit;
		}
		//卖平
		else
		{
			f.LimitPrice = ins.lower_limit;
		}
	}
	//超价
	else if (EPriceType::over == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.bid_price1 + ins.price_tick;
			if (f.LimitPrice > ins.upper_limit)
			{
				f.LimitPrice = ins.upper_limit;
			}
		}
		//卖平
		else
		{
			f.LimitPrice = ins.ask_price1 - ins.price_tick;
			if (f.LimitPrice < ins.lower_limit)
			{
				f.LimitPrice = ins.lower_limit;
			}
		}
	}
	
	f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
	f.ContingentCondition = THOST_FTDC_CC_Immediately;
	f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

	task.has_first_orders_to_send = true;
	CtpActionInsertOrder actionInsertOrder;
	actionInsertOrder.local_key.user_id = _req_login.user_name;
	actionInsertOrder.local_key.order_id = order.order_id + "_closeyestoday_" + std::to_string(nOrderIndex);
	actionInsertOrder.f = f;
	task.first_orders_to_send.push_back(actionInsertOrder);

	task.has_second_orders_to_send = false;

	if (ENeedCancelOrderType::yestoday_buy == needCancelOrderType)
	{
		for (auto it : m_data.m_orders)
		{
			const std::string& orderId = it.first;
			const Order& order = it.second;
			if (order.status == kOrderStatusFinished)
			{
				continue;
			}

			if (order.symbol() != symbol)
			{
				continue;
			}

			if ((order.direction == kDirectionBuy)
				&& (order.offset == kOffsetClose))
			{
				CtpActionCancelOrder cancelOrder;
				cancelOrder.local_key.order_id = orderId;
				cancelOrder.local_key.user_id = _req_login.user_name.c_str();

				CThostFtdcInputOrderActionField f;
				memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
				strcpy_x(f.BrokerID, m_broker_id.c_str());
				strcpy_x(f.UserID, _req_login.user_name.c_str());
				strcpy_x(f.InvestorID, _req_login.user_name.c_str());
				strcpy_x(f.ExchangeID, order.exchange_id.c_str());
				strcpy_x(f.InstrumentID, order.instrument_id.c_str());

				cancelOrder.f = f;

				task.has_order_to_cancel = true;
				task.orders_to_cancel.push_back(cancelOrder);

			}
		}
	}
	else if (ENeedCancelOrderType::yestoday_sell == needCancelOrderType)
	{
		for (auto it : m_data.m_orders)
		{
			const std::string& orderId = it.first;
			const Order& order = it.second;
			if (order.status == kOrderStatusFinished)
			{
				continue;
			}

			if (order.symbol() != symbol)
			{
				continue;
			}

			if ((order.direction == kDirectionSell)
				&& (order.offset == kOffsetClose))
			{
				CtpActionCancelOrder cancelOrder;
				cancelOrder.local_key.order_id = orderId;
				cancelOrder.local_key.user_id = _req_login.user_name.c_str();

				CThostFtdcInputOrderActionField f;
				memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
				strcpy_x(f.BrokerID, m_broker_id.c_str());
				strcpy_x(f.UserID, _req_login.user_name.c_str());
				strcpy_x(f.InvestorID, _req_login.user_name.c_str());
				strcpy_x(f.ExchangeID, order.exchange_id.c_str());
				strcpy_x(f.InstrumentID, order.instrument_id.c_str());

				cancelOrder.f = f;

				task.has_order_to_cancel = true;
				task.orders_to_cancel.push_back(cancelOrder);
			}
		}
	}
	else
	{
		task.has_order_to_cancel = false;
	}

	return true;
}

bool traderctp::ConditionOrder_Close(const ConditionOrder& order
	, const ContingentOrder& co
	, const Instrument& ins
	, ctp_condition_order_task& task
	, int nOrderIndex)
{
	std::string symbol = co.exchange_id + "." + co.instrument_id;

	CThostFtdcInputOrderField f;
	memset(&f, 0, sizeof(CThostFtdcInputOrderField));
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	strcpy_x(f.InvestorID, _req_login.user_name.c_str());
	strcpy_x(f.ExchangeID, co.exchange_id.c_str());
	strcpy_x(f.InstrumentID, co.instrument_id.c_str());

	ENeedCancelOrderType needCancelOrderType = ENeedCancelOrderType::not_need;

	//平仓
	f.CombOffsetFlag[0] = THOST_FTDC_OF_Close;

	//买平
	if (EOrderDirection::buy == co.direction)
	{
		f.Direction = THOST_FTDC_D_Buy;
		Position& pos = GetPosition(symbol);

		//数量类型
		if (EVolumeType::num == co.volume_type)
		{
			//要平的手数小于等于可平手数
			if (co.volume <= pos.volume_short - pos.volume_short_frozen)
			{
				f.VolumeTotalOriginal = co.volume;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			//要平的手数小于等于可平手数(包括冻结的手数)
			else if (co.volume <= pos.volume_short)
			{
				if (order.is_cancel_ori_close_order)
				{
					f.VolumeTotalOriginal = co.volume;
					f.VolumeCondition = THOST_FTDC_VC_AV;
					needCancelOrderType = ENeedCancelOrderType::all_buy;
				}
				else
				{
					Log(LOG_WARNING, nullptr
						, "fun=ConditionOrder_Close;msg=can close short is less than will close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
						, _key.c_str()
						, _req_login.bid.c_str()
						, _req_login.user_name.c_str()
						, co.instrument_id.c_str());
					return false;
				}
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_Close;msg=can close short is less than will close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
		else if (EVolumeType::close_all == co.volume_type)
		{
			Position& position = GetPosition(symbol);
			//如果可平手数大于零
			if (pos.volume_short - pos.volume_short_frozen > 0)
			{
				f.VolumeTotalOriginal = pos.volume_short - pos.volume_short_frozen;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_Close;msg=have no need close short;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
	}
	//卖平
	else
	{
		f.Direction = THOST_FTDC_D_Sell;
		Position& pos = GetPosition(symbol);

		//数量类型
		if (EVolumeType::num == co.volume_type)
		{
			//要平的手数小于等于可平的手数
			if (co.volume <= pos.volume_long - pos.volume_long_frozen)
			{
				f.VolumeTotalOriginal = co.volume;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			//要平的手数小于等于可平手数(包括冻结的手数)
			else if (co.volume <= pos.volume_long)
			{
				if (order.is_cancel_ori_close_order)
				{
					f.VolumeTotalOriginal = co.volume;
					f.VolumeCondition = THOST_FTDC_VC_AV;
					needCancelOrderType = ENeedCancelOrderType::all_sell;
				}
				else
				{
					Log(LOG_WARNING, nullptr
						, "fun=ConditionOrder_Close;msg=can close long is less than will close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
						, _key.c_str()
						, _req_login.bid.c_str()
						, _req_login.user_name.c_str()
						, co.instrument_id.c_str());
					return false;
				}
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_Close;msg=can close long is less than will close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
			}
		}
		else if (EVolumeType::close_all == co.volume_type)
		{
			Position& position = GetPosition(symbol);
			//如果可平手数大于零
			if (pos.volume_long - pos.volume_long_frozen > 0)
			{
				f.VolumeTotalOriginal = pos.volume_long - pos.volume_long_frozen;
				f.VolumeCondition = THOST_FTDC_VC_AV;
			}
			else
			{
				Log(LOG_WARNING, nullptr
					, "fun=ConditionOrder_Close;msg=have no need close long;key=%s;bid=%s;user_name=%s;instrument_id=%s"
					, _key.c_str()
					, _req_login.bid.c_str()
					, _req_login.user_name.c_str()
					, co.instrument_id.c_str());
				return false;
			}
		}
	}

	//价格类型

	//限价
	if (EPriceType::limit == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		f.LimitPrice = co.limit_price;
	}
	//触发价
	else if (EPriceType::contingent == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;

		bool flag = false;
		for (const ContingentCondition& c : order.condition_list)
		{
			if ((c.contingent_type == EContingentType::price)
				&& (c.exchange_id == co.exchange_id)
				&& (c.instrument_id == co.instrument_id))
			{
				f.LimitPrice = c.contingent_price;
				flag = true;
				break;
			}
		}
		if (!flag)
		{
			//找不到触发价
			Log(LOG_WARNING, nullptr
				, "fun=ConditionOrder_CloseYesToday;msg=can not find contingent_price;key=%s;bid=%s;user_name=%s;instrument_id=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, co.instrument_id.c_str());
			return false;
		}
	}
	//对价
	else if (EPriceType::consideration == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.ask_price1;
		}
		//卖平
		else
		{
			f.LimitPrice = ins.bid_price1;
		}
	}
	//市价
	else if (EPriceType::market == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_IOC;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.upper_limit;
		}
		//卖平
		else
		{
			f.LimitPrice = ins.lower_limit;
		}
	}
	//超价
	else if (EPriceType::over == co.price_type)
	{
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_GFD;
		//买平
		if (EOrderDirection::buy == co.direction)
		{
			f.LimitPrice = ins.bid_price1 + ins.price_tick;
			if (f.LimitPrice > ins.upper_limit)
			{
				f.LimitPrice = ins.upper_limit;
			}
		}
		//卖平
		else
		{
			f.LimitPrice = ins.ask_price1 - ins.price_tick;
			if (f.LimitPrice < ins.lower_limit)
			{
				f.LimitPrice = ins.lower_limit;
			}
		}
	}

	f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
	f.ContingentCondition = THOST_FTDC_CC_Immediately;
	f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

	task.has_first_orders_to_send = true;
	CtpActionInsertOrder actionInsertOrder;
	actionInsertOrder.local_key.user_id = _req_login.user_name;
	actionInsertOrder.local_key.order_id = order.order_id + "_close_" + std::to_string(nOrderIndex);
	actionInsertOrder.f = f;
	task.first_orders_to_send.push_back(actionInsertOrder);
	
	task.has_second_orders_to_send = false;

	if (ENeedCancelOrderType::all_buy == needCancelOrderType)
	{
		for (auto it : m_data.m_orders)
		{
			const std::string& orderId = it.first;
			const Order& order = it.second;
			if (order.status == kOrderStatusFinished)
			{
				continue;
			}

			if (order.symbol() != symbol)
			{
				continue;
			}

			if ((order.direction == kDirectionBuy)
				&& ((order.offset == kOffsetClose)||(order.offset == kOffsetCloseToday)))
			{
				CtpActionCancelOrder cancelOrder;
				cancelOrder.local_key.order_id = orderId;
				cancelOrder.local_key.user_id = _req_login.user_name.c_str();

				CThostFtdcInputOrderActionField f;
				memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
				strcpy_x(f.BrokerID, m_broker_id.c_str());
				strcpy_x(f.UserID, _req_login.user_name.c_str());
				strcpy_x(f.InvestorID, _req_login.user_name.c_str());
				strcpy_x(f.ExchangeID, order.exchange_id.c_str());
				strcpy_x(f.InstrumentID, order.instrument_id.c_str());

				cancelOrder.f = f;

				task.has_order_to_cancel = true;
				task.orders_to_cancel.push_back(cancelOrder);

			}
		}
	}
	else if (ENeedCancelOrderType::all_sell == needCancelOrderType)
	{
		for (auto it : m_data.m_orders)
		{
			const std::string& orderId = it.first;
			const Order& order = it.second;
			if (order.status == kOrderStatusFinished)
			{
				continue;
			}

			if (order.symbol() != symbol)
			{
				continue;
			}

			if ((order.direction == kDirectionSell)
				&& ((order.offset == kOffsetClose) || (order.offset == kOffsetCloseToday)))			
			{
				CtpActionCancelOrder cancelOrder;
				cancelOrder.local_key.order_id = orderId;
				cancelOrder.local_key.user_id = _req_login.user_name.c_str();

				CThostFtdcInputOrderActionField f;
				memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
				strcpy_x(f.BrokerID, m_broker_id.c_str());
				strcpy_x(f.UserID, _req_login.user_name.c_str());
				strcpy_x(f.InvestorID, _req_login.user_name.c_str());
				strcpy_x(f.ExchangeID, order.exchange_id.c_str());
				strcpy_x(f.InstrumentID, order.instrument_id.c_str());

				cancelOrder.f = f;

				task.has_order_to_cancel = true;
				task.orders_to_cancel.push_back(cancelOrder);
			}
		}
	}
	else
	{
		task.has_order_to_cancel = false;
	}
	
	return true;
}

bool traderctp::ConditionOrder_Reverse_Long(const ConditionOrder& order
	, const ContingentOrder& co
	, const Instrument& ins
	, ctp_condition_order_task& task
	, int nOrderIndex)
{
	bool b_has_td_yd_distinct = (co.exchange_id == "SHFE") || (co.exchange_id == "INE");
	std::string symbol = co.exchange_id + "." + co.instrument_id;

	//如果有平多单,先撤掉
	for (auto it : m_data.m_orders)
	{
		const std::string& orderId = it.first;
		const Order& order = it.second;
		if (order.status == kOrderStatusFinished)
		{
			continue;
		}

		if (order.symbol() != symbol)
		{
			continue;
		}

		if ((order.direction == kDirectionSell)
			&& ((order.offset == kOffsetClose) || (order.offset == kOffsetCloseToday)))
		{
			CtpActionCancelOrder cancelOrder;
			cancelOrder.local_key.order_id = orderId;
			cancelOrder.local_key.user_id = _req_login.user_name.c_str();

			CThostFtdcInputOrderActionField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, order.exchange_id.c_str());
			strcpy_x(f.InstrumentID, order.instrument_id.c_str());

			cancelOrder.f = f;

			task.has_order_to_cancel = true;
			task.orders_to_cancel.push_back(cancelOrder);
		}
	}

	Position& pos = GetPosition(symbol);

	//重新生成平多单
	
	//如果分今昨
	if (b_has_td_yd_distinct)
	{
		//如果有昨仓
		if (pos.pos_long_his > 0)
		{
			CThostFtdcInputOrderField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, co.exchange_id.c_str());
			strcpy_x(f.InstrumentID, co.instrument_id.c_str());

			//平仓
			f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;

			//卖平
			f.Direction = THOST_FTDC_D_Sell;

			//数量
			f.VolumeTotalOriginal = pos.pos_long_his;
			f.VolumeCondition = THOST_FTDC_VC_AV;

			//价格类型(反手一定用市价平仓)
			f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
			f.TimeCondition = THOST_FTDC_TC_IOC;
			f.LimitPrice = ins.lower_limit;

			f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
			f.ContingentCondition = THOST_FTDC_CC_Immediately;
			f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

			task.has_first_orders_to_send = true;
			CtpActionInsertOrder actionInsertOrder;
			actionInsertOrder.local_key.user_id = _req_login.user_name;
			actionInsertOrder.local_key.order_id = order.order_id + "_closeyestoday_" + std::to_string(nOrderIndex);
			actionInsertOrder.f = f;
			task.first_orders_to_send.push_back(actionInsertOrder);
		}

		if (pos.pos_long_today > 0)
		{
			CThostFtdcInputOrderField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, co.exchange_id.c_str());
			strcpy_x(f.InstrumentID, co.instrument_id.c_str());

			//平仓
			f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;

			//卖平
			f.Direction = THOST_FTDC_D_Sell;

			//数量
			f.VolumeTotalOriginal = pos.pos_long_today;
			f.VolumeCondition = THOST_FTDC_VC_AV;

			//价格类型(反手一定用市价平仓)
			f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
			f.TimeCondition = THOST_FTDC_TC_IOC;
			f.LimitPrice = ins.lower_limit;

			f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
			f.ContingentCondition = THOST_FTDC_CC_Immediately;
			f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

			task.has_first_orders_to_send = true;
			CtpActionInsertOrder actionInsertOrder;
			actionInsertOrder.local_key.user_id = _req_login.user_name;
			actionInsertOrder.local_key.order_id = order.order_id + "_closetoday_" + std::to_string(nOrderIndex);
			actionInsertOrder.f = f;
			task.first_orders_to_send.push_back(actionInsertOrder);
		}
	}
	//如果不分今昨
	else
	{
		//如果有多仓
		if (pos.volume_long > 0)
		{
			CThostFtdcInputOrderField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, co.exchange_id.c_str());
			strcpy_x(f.InstrumentID, co.instrument_id.c_str());

			//平仓
			f.CombOffsetFlag[0] = THOST_FTDC_OF_Close;

			//卖平
			f.Direction = THOST_FTDC_D_Sell;

			//数量
			f.VolumeTotalOriginal = pos.volume_long;
			f.VolumeCondition = THOST_FTDC_VC_AV;

			//价格类型(反手一定用市价平仓)
			f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
			f.TimeCondition = THOST_FTDC_TC_IOC;
			f.LimitPrice = ins.lower_limit;

			f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
			f.ContingentCondition = THOST_FTDC_CC_Immediately;
			f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

			task.has_first_orders_to_send = true;
			CtpActionInsertOrder actionInsertOrder;
			actionInsertOrder.local_key.user_id = _req_login.user_name;
			actionInsertOrder.local_key.order_id = order.order_id + "_close_" + std::to_string(nOrderIndex);
			actionInsertOrder.f = f;
			task.first_orders_to_send.push_back(actionInsertOrder);
		}	
	}

	//生成开空单	
	//如果有多仓
	if (pos.volume_long > 0)
	{
		CThostFtdcInputOrderField f;
		memset(&f, 0, sizeof(CThostFtdcInputOrderField));
		strcpy_x(f.BrokerID, m_broker_id.c_str());
		strcpy_x(f.UserID, _req_login.user_name.c_str());
		strcpy_x(f.InvestorID, _req_login.user_name.c_str());
		strcpy_x(f.ExchangeID, co.exchange_id.c_str());
		strcpy_x(f.InstrumentID, co.instrument_id.c_str());

		//开仓
		f.CombOffsetFlag[0] = THOST_FTDC_OF_Open;

		//开空
		f.Direction = THOST_FTDC_D_Sell;

		//数量
		f.VolumeTotalOriginal = pos.volume_long;
		f.VolumeCondition = THOST_FTDC_VC_AV;

		//价格(反手一定用市价开仓)
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_IOC;
		f.LimitPrice = ins.lower_limit;

		f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
		f.ContingentCondition = THOST_FTDC_CC_Immediately;
		f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

		CtpActionInsertOrder actionInsertOrder;
		actionInsertOrder.local_key.user_id = _req_login.user_name;
		actionInsertOrder.local_key.order_id = order.order_id + "_open_" + std::to_string(nOrderIndex);
		actionInsertOrder.f = f;

		task.has_second_orders_to_send = true;
		task.second_orders_to_send.push_back(actionInsertOrder);
	}
	
	return true;
}

bool traderctp::ConditionOrder_Reverse_Short(const ConditionOrder& order
	, const ContingentOrder& co
	, const Instrument& ins
	, ctp_condition_order_task& task
	, int nOrderIndex)
{
	bool b_has_td_yd_distinct = (co.exchange_id == "SHFE") || (co.exchange_id == "INE");
	std::string symbol = co.exchange_id + "." + co.instrument_id;

	//如果有平空单,先撤掉
	for (auto it : m_data.m_orders)
	{
		const std::string& orderId = it.first;
		const Order& order = it.second;
		if (order.status == kOrderStatusFinished)
		{
			continue;
		}

		if (order.symbol() != symbol)
		{
			continue;
		}
		
		if ((order.direction == kDirectionBuy)
			&& ((order.offset == kOffsetClose) || (order.offset == kOffsetCloseToday)))
		{
			CtpActionCancelOrder cancelOrder;
			cancelOrder.local_key.order_id = orderId;
			cancelOrder.local_key.user_id = _req_login.user_name.c_str();

			CThostFtdcInputOrderActionField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderActionField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, order.exchange_id.c_str());
			strcpy_x(f.InstrumentID, order.instrument_id.c_str());

			cancelOrder.f = f;

			task.has_order_to_cancel = true;
			task.orders_to_cancel.push_back(cancelOrder);
		}

	}

	//重新生成平空单
	Position& pos = GetPosition(symbol);
	//如果分今昨
	if (b_has_td_yd_distinct)
	{
		//如果有昨仓
		if (pos.pos_short_his > 0)
		{
			CThostFtdcInputOrderField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, co.exchange_id.c_str());
			strcpy_x(f.InstrumentID, co.instrument_id.c_str());

			//平仓
			f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;

			//买平
			f.Direction = THOST_FTDC_D_Buy;

			//数量
			f.VolumeTotalOriginal = pos.pos_short_his;
			f.VolumeCondition = THOST_FTDC_VC_AV;

			//价格类型(反手一定用市价平仓)
			f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
			f.TimeCondition = THOST_FTDC_TC_IOC;
			f.LimitPrice = ins.upper_limit;

			f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
			f.ContingentCondition = THOST_FTDC_CC_Immediately;
			f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

			task.has_first_orders_to_send = true;
			CtpActionInsertOrder actionInsertOrder;
			actionInsertOrder.local_key.user_id = _req_login.user_name;
			actionInsertOrder.local_key.order_id = order.order_id + "_closeyestoday_" + std::to_string(nOrderIndex);
			actionInsertOrder.f = f;
			task.first_orders_to_send.push_back(actionInsertOrder);
		}

		//如果有今仓
		if (pos.pos_short_today > 0)
		{
			CThostFtdcInputOrderField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, co.exchange_id.c_str());
			strcpy_x(f.InstrumentID, co.instrument_id.c_str());

			//平仓
			f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;

			//买平
			f.Direction = THOST_FTDC_D_Buy;

			//数量
			f.VolumeTotalOriginal = pos.pos_short_today;
			f.VolumeCondition = THOST_FTDC_VC_AV;

			//价格类型(反手一定用市价平仓)
			f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
			f.TimeCondition = THOST_FTDC_TC_IOC;
			f.LimitPrice = ins.upper_limit;

			f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
			f.ContingentCondition = THOST_FTDC_CC_Immediately;
			f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

			task.has_first_orders_to_send = true;
			CtpActionInsertOrder actionInsertOrder;
			actionInsertOrder.local_key.user_id = _req_login.user_name;
			actionInsertOrder.local_key.order_id = order.order_id + "_closetoday_" + std::to_string(nOrderIndex);
			actionInsertOrder.f = f;
			task.first_orders_to_send.push_back(actionInsertOrder);
		}
	}
	//如果不分今昨
	else
	{
		//如果有空仓
		if (pos.volume_short > 0)
		{
			CThostFtdcInputOrderField f;
			memset(&f, 0, sizeof(CThostFtdcInputOrderField));
			strcpy_x(f.BrokerID, m_broker_id.c_str());
			strcpy_x(f.UserID, _req_login.user_name.c_str());
			strcpy_x(f.InvestorID, _req_login.user_name.c_str());
			strcpy_x(f.ExchangeID, co.exchange_id.c_str());
			strcpy_x(f.InstrumentID, co.instrument_id.c_str());

			//平仓
			f.CombOffsetFlag[0] = THOST_FTDC_OF_Close;

			//买平
			f.Direction = THOST_FTDC_D_Buy;

			//数量
			f.VolumeTotalOriginal = pos.volume_short;
			f.VolumeCondition = THOST_FTDC_VC_AV;

			//价格类型(反手一定用市价平仓)
			f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
			f.TimeCondition = THOST_FTDC_TC_IOC;
			f.LimitPrice = ins.upper_limit;

			f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
			f.ContingentCondition = THOST_FTDC_CC_Immediately;
			f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

			task.has_first_orders_to_send = true;
			CtpActionInsertOrder actionInsertOrder;
			actionInsertOrder.local_key.user_id = _req_login.user_name;
			actionInsertOrder.local_key.order_id = order.order_id + "_close_" + std::to_string(nOrderIndex);
			actionInsertOrder.f = f;
			task.first_orders_to_send.push_back(actionInsertOrder);
		}
	}

	//生成开多单	

	//如果有空仓
	if (pos.volume_short > 0)
	{
		CThostFtdcInputOrderField f;
		memset(&f, 0, sizeof(CThostFtdcInputOrderField));
		strcpy_x(f.BrokerID, m_broker_id.c_str());
		strcpy_x(f.UserID, _req_login.user_name.c_str());
		strcpy_x(f.InvestorID, _req_login.user_name.c_str());
		strcpy_x(f.ExchangeID, co.exchange_id.c_str());
		strcpy_x(f.InstrumentID, co.instrument_id.c_str());

		//开仓
		f.CombOffsetFlag[0] = THOST_FTDC_OF_Open;

		//开空
		f.Direction = THOST_FTDC_D_Buy;

		//数量
		f.VolumeTotalOriginal = pos.volume_short;
		f.VolumeCondition = THOST_FTDC_VC_AV;

		//价格(反手一定用市价开仓)
		f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
		f.TimeCondition = THOST_FTDC_TC_IOC;
		f.LimitPrice = ins.upper_limit;

		f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
		f.ContingentCondition = THOST_FTDC_CC_Immediately;
		f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

		CtpActionInsertOrder actionInsertOrder;
		actionInsertOrder.local_key.user_id = _req_login.user_name;
		actionInsertOrder.local_key.order_id = order.order_id + "_open_" + std::to_string(nOrderIndex);
		actionInsertOrder.f = f;

		task.has_second_orders_to_send = true;
		task.second_orders_to_send.push_back(actionInsertOrder);
	}
	
	return true;
}

void traderctp::OnTouchConditionOrder(const ConditionOrder& order)
{
	SerializerConditionOrderData ss;
	ss.FromVar(order);
	std::string strMsg;
	ss.ToString(&strMsg);
	Log(LOG_INFO,strMsg.c_str()
		, "fun=OnTouchConditionOrder;msg=condition order is touched;key=%s;bid=%s;user_name=%s"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str());

	int nOrderIndex = 0;
	for (const ContingentOrder& co : order.order_list)
	{
		nOrderIndex++;
		ctp_condition_order_task task;
		std::string symbol = co.exchange_id + "." + co.instrument_id;
		Instrument* ins = GetInstrument(symbol);
		if (nullptr==ins)
		{
			Log(LOG_WARNING, nullptr
				, "fun=OnTouchConditionOrder;msg=instrument not exist;key=%s;bid=%s;user_name=%s;symbol=%s"
				, _key.c_str()
				, _req_login.bid.c_str()
				, _req_login.user_name.c_str()
				, symbol.c_str());
			continue;
		}
		
		bool flag = false;
		//如果是开仓
		if (EOrderOffset::open == co.offset)
		{
			flag=ConditionOrder_Open(order,co,*ins,task,nOrderIndex);				
		}
		//平今
		else if (EOrderOffset::close_today == co.offset)
		{			
			flag = ConditionOrder_CloseToday(order,co,*ins,task,nOrderIndex);		
		}
		else if (EOrderOffset::close == co.offset)
		{
			bool b_has_td_yd_distinct = (co.exchange_id == "SHFE") || (co.exchange_id == "INE");
			if (b_has_td_yd_distinct)
			{
				//平昨
				flag = ConditionOrder_CloseYesToday(order,co,*ins,task,nOrderIndex);
			}
			else
			{
				//不分今昨的平仓				
				flag = ConditionOrder_Close(order,co,*ins,task,nOrderIndex);
			}			
		}		
		else if (EOrderOffset::reverse == co.offset)
		{
			//对空头进行反手操作
			if (co.direction == EOrderDirection::buy)
			{
				flag = ConditionOrder_Reverse_Short(order, co, *ins, task, nOrderIndex);
			}
			//对多头进行反手操作
			else if (co.direction == EOrderDirection::sell)
			{
				flag = ConditionOrder_Reverse_Long(order, co, *ins, task, nOrderIndex);
			}
		}

		if (!flag)
		{
			continue;
		}

		//开始发单
		if (task.has_order_to_cancel)
		{
			for (auto oc : task.orders_to_cancel)
			{
				OnConditionOrderReqCancelOrder(oc);
			}
			m_condition_order_task.push_back(task);
			continue;
		}
		else if (task.has_first_orders_to_send)
		{
			for (auto o : task.first_orders_to_send)
			{
				OnConditionOrderReqInsertOrder(o);
			}
			m_condition_order_task.push_back(task);
			continue;
		}
		else if (task.has_second_orders_to_send)
		{
			for (auto o : task.second_orders_to_send)
			{
				OnConditionOrderReqInsertOrder(o);
			}
			m_condition_order_task.push_back(task);
			continue;
		}
	}	
}

void traderctp::CheckConditionOrderCancelOrderTask(const std::string& orderId)
{
	for (ctp_condition_order_task& task : m_condition_order_task)
	{
		if (!task.has_order_to_cancel)
		{
			continue;
		}

		bool flag = false;
		for (auto it = task.orders_to_cancel.begin(); it != task.orders_to_cancel.end(); it++)
		{
			if (it->local_key.order_id == orderId)
			{
				task.orders_to_cancel.erase(it);
				flag = true;
				break;
			}			
		}

		if (flag)
		{
			//撤单已经完成
			if (task.orders_to_cancel.empty())
			{
				task.has_order_to_cancel = false;

				if (task.has_first_orders_to_send)
				{
					for (auto o : task.first_orders_to_send)
					{
						OnConditionOrderReqInsertOrder(o);
					}
				}
				else if (task.has_second_orders_to_send)
				{
					for (auto o : task.second_orders_to_send)
					{
						OnConditionOrderReqInsertOrder(o);
					}					
				}
			}

			break;
		}
	}	
}

void traderctp::CheckConditionOrderSendOrderTask(const std::string& orderId)
{
	for (ctp_condition_order_task& task : m_condition_order_task)
	{
		if (task.has_order_to_cancel)
		{
			continue;
		}

		if (task.has_first_orders_to_send)
		{
			bool flag = false;
			for (auto it = task.first_orders_to_send.begin(); it != task.first_orders_to_send.end(); it++)
			{
				if (it->local_key.order_id == orderId)
				{
					task.first_orders_to_send.erase(it);
					flag = true;
					break;
				}
			}

			if (flag)
			{
				//第一批Order已经成交
				if (task.first_orders_to_send.empty())
				{
					task.has_first_orders_to_send = false;

					//发送第二批Order
					if (task.has_second_orders_to_send)
					{
						for (auto o : task.second_orders_to_send)
						{
							OnConditionOrderReqInsertOrder(o);
						}
					}
					
				}


				break;
			}
			else
			{
				continue;
			}			
		}

		if (task.has_second_orders_to_send)
		{
			bool flag = false;
			for (auto it = task.second_orders_to_send.begin(); it != task.second_orders_to_send.end(); it++)
			{
				if (it->local_key.order_id == orderId)
				{
					task.second_orders_to_send.erase(it);
					flag = true;
					break;
				}
			}

			if (flag)
			{
				//第二批Order已经成交
				if (task.second_orders_to_send.empty())
				{
					task.has_second_orders_to_send = false;
				}
				break;
			}
			else
			{
				continue;
			}
		}

	}

	for (auto it = m_condition_order_task.begin(); it != m_condition_order_task.end();)
	{
		ctp_condition_order_task& task = *it;
		if (!task.has_order_to_cancel
			&& !task.has_first_orders_to_send
			&& !task.has_second_orders_to_send)
		{
			it = m_condition_order_task.erase(it);
		}
		else
		{
			it++;
		}
	}
}

void traderctp::OnConditionOrderReqCancelOrder(CtpActionCancelOrder& d)
{
	RemoteOrderKey rkey;
	if (!OrderIdLocalToRemote(d.local_key, &rkey))
	{
		Log(LOG_WARNING, nullptr
			, "fun=OnConditionOrderReqCancelOrder;msg=orderid is not exist;key=%s;bid=%s;user_name=%s;orderid=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str()
			, d.local_key.order_id.c_str());
		return;
	}

	strcpy_x(d.f.BrokerID, m_broker_id.c_str());
	strcpy_x(d.f.UserID, _req_login.user_name.c_str());
	strcpy_x(d.f.InvestorID, _req_login.user_name.c_str());
	strcpy_x(d.f.OrderRef, rkey.order_ref.c_str());
	strcpy_x(d.f.ExchangeID, rkey.exchange_id.c_str());
	strcpy_x(d.f.InstrumentID, rkey.instrument_id.c_str());

	d.f.SessionID = rkey.session_id;
	d.f.FrontID = rkey.front_id;
	d.f.ActionFlag = THOST_FTDC_AF_Delete;
	d.f.LimitPrice = 0;
	d.f.VolumeChange = 0;
	{
		m_cancel_order_set.insert(d.local_key.order_id);
	}

	std::stringstream ss;
	ss << m_front_id << m_session_id << d.f.OrderRef;
	std::string strKey = ss.str();
	m_action_order_map.insert(
		std::map<std::string, std::string>::value_type(strKey, strKey));

	int r = m_pTdApi->ReqOrderAction(&d.f, 0);
	if (0 != r)
	{
		OutputNotifyAllSycn(1, u8"撤单请求发送失败!", "WARNING");
		Log(LOG_WARNING, nullptr
			, "fun=OnConditionOrderReqCancelOrder;msg=cancel order request is fail;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
	}
	Log(LOG_INFO, nullptr
		, "fun=OnConditionOrderReqCancelOrder;key=%s;bid=%s;user_name=%s;InstrumentID=%s;OrderRef=%s;ret=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, d.f.InstrumentID
		, d.f.OrderRef
		, r);
}

void traderctp::OnConditionOrderReqInsertOrder(CtpActionInsertOrder& d)
{	
	RemoteOrderKey rkey;
	rkey.exchange_id = d.f.ExchangeID;
	rkey.instrument_id = d.f.InstrumentID;
	if (OrderIdLocalToRemote(d.local_key, &rkey))
	{
		Log(LOG_WARNING, nullptr
			, "fun=OnConditionOrderReqInsertOrder;msg=orderid is duplicate,can not send order;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
		return;
	}

	strcpy_x(d.f.OrderRef, rkey.order_ref.c_str());
	{
		m_insert_order_set.insert(d.f.OrderRef);
	}

	std::stringstream ss;
	ss << m_front_id << m_session_id << d.f.OrderRef;
	std::string strKey = ss.str();
	ServerOrderInfo serverOrder;
	serverOrder.InstrumentId = rkey.instrument_id;
	serverOrder.ExchangeId = rkey.exchange_id;
	serverOrder.VolumeOrigin = d.f.VolumeTotalOriginal;
	serverOrder.VolumeLeft = d.f.VolumeTotalOriginal;
	m_input_order_key_map.insert(std::map<std::string
		, ServerOrderInfo>::value_type(strKey, serverOrder));
	int r = m_pTdApi->ReqOrderInsert(&d.f, 0);
	if (0 != r)
	{		
		Log(LOG_WARNING, nullptr
			, "fun=OnClientReqInsertConditionOrder;msg=send order request is fail;key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
	}
	Log(LOG_INFO, nullptr
		, "fun=OnClientReqInsertConditionOrder;key=%s;orderid=%s;bid=%s;user_name=%s;InstrumentID=%s;OrderRef=%s;ret=%d;OrderPriceType=%c;Direction=%c;CombOffsetFlag=%c;LimitPrice=%f;VolumeTotalOriginal=%d;VolumeCondition=%c;TimeCondition=%c"
		, _key.c_str()
		, d.local_key.order_id.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, d.f.InstrumentID
		, d.f.OrderRef
		, r
		, d.f.OrderPriceType
		, d.f.Direction
		, d.f.CombHedgeFlag[0]
		, d.f.LimitPrice
		, d.f.VolumeTotalOriginal
		, d.f.VolumeCondition
		, d.f.TimeCondition);
	m_need_save_file.store(true);
}

#pragma endregion

#pragma region ctpse

int traderctp::RegSystemInfo()
{
	return 0;
}

int traderctp::ReqAuthenticate()
{
	if (m_try_req_authenticate_times > 0)
	{
		int nSeconds = 10 + m_try_req_authenticate_times * 1;
		if (nSeconds > 60)
		{
			nSeconds = 60;
		}
		boost::this_thread::sleep_for(boost::chrono::seconds(nSeconds));
	}

	m_try_req_authenticate_times++;
	if (_req_login.broker.auth_code.empty())
	{
		Log(LOG_INFO, nullptr
			, "fun=ReqAuthenticate;msg=_req_login.broker.auth_code.empty();key=%s;bid=%s;user_name=%s"
			, _key.c_str()
			, _req_login.bid.c_str()
			, _req_login.user_name.c_str());
		SendLoginRequest();
		return 0;
	}

	CThostFtdcReqAuthenticateField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.UserID, _req_login.user_name.c_str());
	strcpy_x(field.UserProductInfo, _req_login.broker.product_info.c_str());
	strcpy_x(field.AuthCode, _req_login.broker.auth_code.c_str());
	int ret = m_pTdApi->ReqAuthenticate(&field, ++_requestID);
	Log(LOG_INFO, nullptr
		, "fun=ReqAuthenticate;msg=ctp ReqAuthenticate;key=%s;bid=%s;user_name=%s;UserProductInfo=%s;AuthCode=%s;ret=%d"
		, _key.c_str()
		, _req_login.bid.c_str()
		, _req_login.user_name.c_str()
		, _req_login.broker.product_info.c_str()
		, _req_login.broker.auth_code.c_str()
		, ret);
	return ret;
}

#pragma endregion
