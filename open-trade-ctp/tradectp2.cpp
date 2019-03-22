/////////////////////////////////////////////////////////////////////////
///@file tradectp2.cpp
///@brief	CTP交易逻辑实现
///@copyright	上海信易信息科技股份有限公司 版权所有
/////////////////////////////////////////////////////////////////////////

#include "tradectp.h"
#include "ctp_define.h"
#include "SerializerTradeBase.h"
#include "config.h"
#include "utility.h"

#include <iostream>
#include <string>
#include <fstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

using namespace trader_dll;

traderctp::traderctp(boost::asio::io_context& ios
	,const std::string& logFileName)
	:m_b_login(false)
	,_logFileName(logFileName)
	,m_settlement_info("")
	,_ios(ios)
	,_out_mq_ptr()
	,_out_mq_name(logFileName+"_msg_out")
	,_in_mq_ptr()
	,_in_mq_name(logFileName + "_msg_in")
	,_thread_ptr()
	,_ios_out()
	,_woker_out(_ios_out)
	,_thread_out(boost::bind(&boost::asio::io_context::run,&_ios_out))
	,m_notify_seq(0)
	,m_data_seq(0)
	,_req_login()
	,m_broker_id("")
	,m_pTdApi(NULL)		
	,m_trading_day("")
	,m_front_id(0)
	,m_session_id(0)
	,m_order_ref(0)	
	,m_input_order_key_map()
	,m_action_order_map()
	,_logIn(false)
	,_logInmutex()
	,_logInCondition()
	,m_loging_connectId(-1)
	,m_logined_connIds()
	,m_user_file_path("")
	,m_ordermap_local_remote()
	,m_ordermap_remote_local()	
	,m_data()
{
	m_req_login_dt = 0;
	m_next_qry_dt = 0;
	m_next_send_dt = 0;
	
	m_need_query_settlement.store(false);
	m_req_account_id.store(0);

	m_req_position_id.store(0);	
	m_rsp_position_id.store(0);

	m_rsp_account_id.store(0);
	m_need_query_bank.store(false);
	m_need_query_register.store(false);	
	m_position_ready.store(false);

	m_something_changed = false;
	m_peeking_message = false;
}

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
		Log(LOG_ERROR, NULL, "Open message_queue Erro:%s", ex.what());
	}

	_thread_ptr = boost::make_shared<boost::thread>(
		boost::bind(&traderctp::ReceiveMsg, this));
}

void traderctp::ReceiveMsg()
{
	char buf[MAX_MSG_LENTH];
	unsigned int priority;
	boost::interprocess::message_queue::size_type recvd_size;
	while (true)
	{
		try
		{
			memset(buf, 0, sizeof(buf));
			boost::posix_time::ptime tm = boost::get_system_time()
				+ boost::posix_time::milliseconds(100);
			bool flag=_in_mq_ptr->timed_receive(buf, sizeof(buf),recvd_size, priority,tm);
			if (!flag)
			{
				_ios.post(boost::bind(&traderctp::OnIdle,this));				
				continue;
			}
			std::string line=buf;
			if (line.empty())
			{				
				continue;
			}			
			std::vector<std::string> items;
			boost::algorithm::split(items, line, boost::algorithm::is_any_of("|"));
			int connId = -1;
			std::string msg = "";
			if (items.size() == 1)
			{
				msg = items[0];
			}
			else if (items.size() == 2)
			{
				connId = atoi(items[0].c_str());
				msg = items[1];
			}
			else
			{
				Log(LOG_WARNING, NULL,
					"traderctp ReceiveMsg:%s is invalid!"
					, line.c_str());
				continue;
			}
			std::shared_ptr<std::string> msg_ptr(new std::string(msg));
			_ios.post(boost::bind(&traderctp::ProcessInMsg
				,this,connId,msg_ptr));
		}
		catch (const std::exception& ex)
		{
			Log(LOG_ERROR, NULL, "ReceiveMsg_i Erro:%s", ex.what());
		}
	}	
}

void traderctp::Stop()
{
	if (nullptr != _thread_ptr)
	{
		_thread_ptr->detach();
		_thread_ptr.reset();
	}
	
	StopTdApi();

	_ios_out.stop();
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
	Log(LOG_WARNING, NULL,"CloseConnection:%d",nId);
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

void traderctp::ProcessInMsg(int connId,std::shared_ptr<std::string> msg_ptr)
{
	if (nullptr == msg_ptr)
	{
		return;
	}
	std::string& msg = *msg_ptr;

	//一个特殊的消息
	if (connId == -1)
	{
		int nCloseConnection = atoi(msg.c_str());		
		CloseConnection(nCloseConnection);
		return;
	}

	SerializerTradeBase ss;
	if (!ss.FromString(msg.c_str()))
	{
		Log(LOG_WARNING, NULL, "trade ctp parse json(%s) fail", msg.c_str());
		return;
	}

	ReqLogin req;
	ss.ToVar(req);
	if (req.aid == "req_login")
	{
		ProcessReqLogIn(connId,req);
	}
	else
	{
		if (!m_b_login)
		{
			Log(LOG_WARNING, NULL, "trade ctp receive other msg before login:%s"
				,msg.c_str());
			return;
		}
		
		if (!IsConnectionLogin(connId))
		{
			Log(LOG_WARNING, NULL, "trade ctp receive other msg which from not login connecion:%s"
				, msg.c_str());
			return;
		}

		SerializerCtp ss;
		if (!ss.FromString(msg.c_str()))
			return;

		rapidjson::Value* dt = rapidjson::Pointer("/aid").Get(*(ss.m_doc));
		if (!dt || !dt->IsString())
			return;

		std::string aid = dt->GetString();
		if (aid == "peek_message") 
		{
			OnClientPeekMessage();
		}
		else if (aid == "insert_order") 
		{
			CtpActionInsertOrder d;
			ss.ToVar(d);
			OnClientReqInsertOrder(d);
		}
		else if (aid == "cancel_order") 
		{
			CtpActionCancelOrder d;
			ss.ToVar(d);
			OnClientReqCancelOrder(d);
		}
		else if (aid == "req_transfer") 
		{
			CThostFtdcReqTransferField f;
			memset(&f, 0, sizeof(f));
			ss.ToVar(f);
			OnClientReqTransfer(f);
		}
		else if (aid == "confirm_settlement") 
		{			
			ReqConfirmSettlement();
		}
		else if (aid == "change_password") 
		{
			CThostFtdcUserPasswordUpdateField f;
			memset(&f, 0, sizeof(f));
			ss.ToVar(f);
			OnClientReqChangePassword(f);
		}
	}	
}

void traderctp::OnClientReqChangePassword(CThostFtdcUserPasswordUpdateField f)
{
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	int r = m_pTdApi->ReqUserPasswordUpdate(&f, 0);
	Log(LOG_INFO, NULL, "ctp ReqUserPasswordUpdate, instance=%p, ret=%d"
		, this, r);
}

void traderctp::OnClientReqTransfer(CThostFtdcReqTransferField f)
{
	strcpy_x(f.BrokerID, m_broker_id.c_str());
	strcpy_x(f.UserID, _req_login.user_name.c_str());
	strcpy_x(f.AccountID,_req_login.user_name.c_str());
	strcpy_x(f.BankBranchID, "0000");
	f.SecuPwdFlag = THOST_FTDC_BPWDF_BlankCheck;	// 核对密码
	f.BankPwdFlag = THOST_FTDC_BPWDF_NoCheck;	// 核对密码
	f.VerifyCertNoFlag = THOST_FTDC_YNI_No;
	if (f.TradeAmount >= 0) 
	{
		strcpy_x(f.TradeCode, "202001");
		int r = m_pTdApi->ReqFromBankToFutureByFuture(&f, 0);
		Log(LOG_INFO, NULL, "ctp ReqFromBankToFutureByFuture, instance=%p, UserID=%s, TradeAmount=%f, ret=%d"
			, this,f.UserID, f.TradeAmount, r);
	}
	else
	{
		strcpy_x(f.TradeCode, "202002");
		f.TradeAmount = -f.TradeAmount;
		int r = m_pTdApi->ReqFromFutureToBankByFuture(&f, 0);
		Log(LOG_INFO, NULL, "ctp ReqFromFutureToBankByFuture, instance=%p, UserID=%s, TradeAmount=%f, ret=%d"
			, this, f.UserID, f.TradeAmount, r);
	}
}

void traderctp::OnClientReqCancelOrder(CtpActionCancelOrder d)
{
	if (d.local_key.user_id.substr(0, _req_login.user_name.size()) != _req_login.user_name)
	{
		OutputNotifyAllSycn(1,GBKToUTF8("撤单user_id错误，不能撤单"), "WARNING");
		return;
	}

	RemoteOrderKey rkey;
	if (!OrderIdLocalToRemote(d.local_key, &rkey)) 
	{
		OutputNotifyAllSycn(1,GBKToUTF8("撤单指定的order_id不存在，不能撤单"), "WARNING");
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
	Log(LOG_INFO, NULL, "ctp ReqOrderAction, instance=%p, InvestorID=%s, InstrumentID=%s, OrderRef=%s, ret=%d"
		, this, d.f.InvestorID, d.f.InstrumentID, d.f.OrderRef, r);
}

void traderctp::OnClientReqInsertOrder(CtpActionInsertOrder d)
{
	if (d.local_key.user_id.substr(0,_req_login.user_name.size()) != _req_login.user_name)
	{
		OutputNotifyAllSycn(1,GBKToUTF8("报单user_id错误，不能下单"), "WARNING");
		return;
	}
	strcpy_x(d.f.BrokerID,m_broker_id.c_str());
	strcpy_x(d.f.UserID,_req_login.user_name.c_str());
	strcpy_x(d.f.InvestorID,_req_login.user_name.c_str());
	RemoteOrderKey rkey;
	rkey.exchange_id = d.f.ExchangeID;
	rkey.instrument_id = d.f.InstrumentID;
	if (OrderIdLocalToRemote(d.local_key, &rkey)) 
	{
		OutputNotifyAllSycn(1,GBKToUTF8("报单单号重复，不能下单"),"WARNING");
		return;
	}

	strcpy_x(d.f.OrderRef,rkey.order_ref.c_str());
	{		
		m_insert_order_set.insert(d.f.OrderRef);
	}

	std::stringstream ss;
	ss << m_front_id << m_session_id << d.f.OrderRef;
	std::string strKey = ss.str();
	m_input_order_key_map.insert(
		std::map<std::string, std::string>::value_type(strKey,strKey));
	
	int r = m_pTdApi->ReqOrderInsert(&d.f, 0);
	Log(LOG_INFO, NULL
		, "ctp ReqOrderInsert, instance=%p, InvestorID=%s, InstrumentID=%s, OrderRef=%s, ret=%d"
		, this
		, d.f.InvestorID
		, d.f.InstrumentID
		, d.f.OrderRef,r);
	SaveToFile();
}

void traderctp::OnClientPeekMessage()
{
	m_peeking_message = true;
	//向客户端发送账户信息
	SendUserData();
}

void traderctp::ProcessReqLogIn(int connId,ReqLogin& req)
{
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
			//加入登录客户端列表
			m_logined_connIds.push_back(connId);

			OutputNotifySycn(connId,0,u8"登录成功");
			char json_str[1024];
			sprintf(json_str, (u8"{"\
				"\"aid\": \"rtn_data\","\
				"\"data\" : [{\"trade\":{\"%s\":{\"session\":{"\
				"\"user_id\" : \"%s\","\
				"\"trading_day\" : \"%s\""
				"}}}}]}")
				, _req_login.user_name.c_str()
				, _req_login.user_name.c_str()
				, m_trading_day.c_str()
			);

			std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
			_ios_out.post(boost::bind(&traderctp::SendMsg, this, connId, msg_ptr));
						
			//发送用户数据
			SendUserDataImd(connId);			
		}
		else
		{
			OutputNotifySycn(connId,0,u8"用户登录失败!");
		}
	}
	else
	{
		_req_login = req;
		auto it = g_config.brokers.find(_req_login.bid);
		_req_login.broker = it->second;

		if (!g_config.user_file_path.empty())
		{
			m_user_file_path = g_config.user_file_path + "/" + _req_login.bid;
		}			
		m_data.user_id = _req_login.user_name;
		LoadFromFile();
		m_loging_connectId = connId;
		InitTdApi();	
		bool login = WaitLogIn();
		m_b_login.store(login);
		if (m_b_login.load())
		{
			//加入登录客户端列表
			m_logined_connIds.push_back(connId);					
			char json_str[1024];
			sprintf(json_str, (u8"{"\
				"\"aid\": \"rtn_data\","\
				"\"data\" : [{\"trade\":{\"%s\":{\"session\":{"\
				"\"user_id\" : \"%s\","\
				"\"trading_day\" : \"%s\""
				"}}}}]}")
				,_req_login.user_name.c_str()
				,_req_login.user_name.c_str()
				,m_trading_day.c_str()
			);
			std::shared_ptr<std::string> msg_ptr(new std::string(json_str));
			_ios_out.post(boost::bind(&traderctp::SendMsg,this,connId, msg_ptr));			
		}
		else
		{
			OutputNotifySycn(connId,0,u8"用户登录失败!");
			StopTdApi();			
		}
	}	
}

bool traderctp::WaitLogIn()
{
	boost::unique_lock<boost::mutex> lock(_logInmutex);
	_logIn = false;
	m_pTdApi->Init();	
	_logInCondition.timed_wait(lock, boost::posix_time::seconds(15));
	return _logIn;
}

void traderctp::InitTdApi()
{
	std::string flow_file_name = GenerateUniqFileName();	
	Log(LOG_WARNING, NULL, "flow_file_name:%s",flow_file_name.c_str());
	m_pTdApi = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_file_name.c_str());
	m_pTdApi->RegisterSpi(this);
	m_broker_id = _req_login.broker.ctp_broker_id;
	for (auto it = _req_login.broker.trading_fronts.begin()
		; it != _req_login.broker.trading_fronts.end(); ++it)
	{
		std::string& f = *it;
		Log(LOG_WARNING, NULL,"front:%s",f.c_str());
		m_pTdApi->RegisterFront((char*)(f.c_str()));
	}	
	m_pTdApi->SubscribePrivateTopic(THOST_TERT_RESUME);
	m_pTdApi->SubscribePublicTopic(THOST_TERT_RESUME);		
}

void traderctp::StopTdApi()
{
	if (nullptr != m_pTdApi)
	{
		Log(LOG_INFO, NULL, "ctp OnFinish, instance=%p, UserId=%s", this
			, _req_login.user_name.c_str());
		m_pTdApi->RegisterSpi(NULL);
		m_pTdApi->Release();
		m_pTdApi = NULL;		
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
	_ios_out.post(boost::bind(&traderctp::SendMsg,this,connId, msg_ptr));
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
	_ios_out.post(boost::bind(&traderctp::SendMsg, this,connId, msg_ptr));
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
		_ios_out.post(boost::bind(&traderctp::SendMsgAll,this,conn_ptr,msg_ptr));
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
		_ios_out.post(boost::bind(&traderctp::SendMsgAll, this, conn_ptr, msg_ptr));
	}
}

void traderctp::SendMsgAll(std::shared_ptr<std::string> conn_str_ptr,std::shared_ptr<std::string> msg_ptr)
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
		
	int length = MAX_MSG_LENTH - 16- conn_str.length();
	if (msg.length() > length)
	{
		try
		{
			std::vector<std::string> vecs;
			SplitString(msg, vecs, length);
			int flag = 0;
			for (int i = 0; i < vecs.size(); ++i)
			{
				//最后一个
				if ((i + 1) == vecs.size())
				{
					flag = 1;
				}
				std::stringstream ss;
				ss << conn_str << "#" << flag << "#" << vecs[i];
				std::string str = ss.str();
				//Log(LOG_ERROR, NULL, "SendMsgAll 1 msg:%s"
				//, str.c_str());
				_out_mq_ptr->send(str.c_str(), str.length(), 0);
			}
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR, NULL, "SendMsg Erro:%s,msg:%s,length:%d"
				, ex.what(), msg.c_str(), msg.length());
		}
	}
	else
	{
		try
		{
			std::stringstream ss;
			ss << conn_str << "#" << msg;
			std::string str = ss.str();
			//Log(LOG_ERROR, NULL, "SendMsgAll 2 msg:%s"
			//	, str.c_str());
			_out_mq_ptr->send(str.c_str(), str.length(), 0);
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR, NULL, "SendMsg Erro:%s,msg:%s,length:%d"
				, ex.what(), msg.c_str(), msg.length());
		}
	}
}

void traderctp::SendMsg(int connId,std::shared_ptr<std::string> msg_ptr)
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

	//Log(LOG_ERROR, NULL, "SendMsg connid:%d msg:%s"
	//	,connId,msg.c_str());

	int length = MAX_MSG_LENTH - 16;
	if (msg.length() > length)
	{
		try
		{
			std::vector<std::string> vecs;
			SplitString(msg, vecs, length);
			int flag = 0;
			for (int i = 0; i < vecs.size(); ++i)
			{
				//最后一个
				if ((i + 1) == vecs.size())
				{
					flag = 1;
				}
				std::stringstream ss;
				ss << connId << "#" << flag << "#" << vecs[i];
				std::string str = ss.str();
				_out_mq_ptr->send(str.c_str(), str.length(), 0);
			}
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR, NULL, "SendMsg Erro:%s,msg:%s,length:%d"
				, ex.what(), msg.c_str(), msg.length());
		}
	}
	else
	{
		try
		{
			std::stringstream ss;
			ss << connId << "#" << msg;
			std::string str = ss.str();
			_out_mq_ptr->send(str.c_str(), str.length(), 0);
		}
		catch (std::exception& ex)
		{
			Log(LOG_ERROR, NULL, "SendMsg Erro:%s,msg:%s,length:%d"
				, ex.what(), msg.c_str(), msg.length());
		}
	}
}