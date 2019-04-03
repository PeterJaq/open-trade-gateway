/////////////////////////////////////////////////////////////////////////
///@file user_process_info.cpp
///@brief	用户进程管理类
///@copyright	上海信易信息科技股份有限公司 版权所有 
/////////////////////////////////////////////////////////////////////////

#include "user_process_info.h"
#include "SerializerTradeBase.h"

#include <boost/algorithm/string.hpp>

using namespace std::chrono;

UserProcessInfo::UserProcessInfo(boost::asio::io_context& ios
	, const ReqLogin& reqLogin)
	:io_context_(ios)
	, _out_mq_ptr()
	, _out_mq_name("")
	, _thread_ptr()
	,_in_mq_ptr()
	,_in_mq_name("")	
	,_process_ptr()	
	,user_connections_()
	,_reqLogin(reqLogin)	
{
}

bool UserProcessInfo::ProcessIsRunning()
{
	if (nullptr == _process_ptr)
	{
		return false;
	}
	return _process_ptr->running();
}

bool UserProcessInfo::StartProcess()
{
	try
	{
		if(_reqLogin.broker.broker_type == "ctp")
		{			
			std::string cmd = "ctp_" + _reqLogin.bid + "_" + _reqLogin.user_name;	

			_out_mq_name = cmd + "_msg_out";
			_in_mq_name = cmd + "_msg_in";
			
			boost::interprocess::message_queue::remove(_out_mq_name.c_str());
			boost::interprocess::message_queue::remove(_in_mq_name.c_str());

			_out_mq_ptr = std::shared_ptr <boost::interprocess::message_queue>
				(new boost::interprocess::message_queue(boost::interprocess::create_only
					, _out_mq_name.c_str(),MAX_MSG_NUMS,MAX_MSG_LENTH));
			_thread_ptr.reset();
			
			_thread_ptr = std::shared_ptr<boost::thread>(
				new boost::thread(boost::bind(&UserProcessInfo::ReceiveMsg_i,this)));

			_in_mq_ptr= std::shared_ptr <boost::interprocess::message_queue>
				(new boost::interprocess::message_queue(boost::interprocess::create_only
					,_in_mq_name.c_str(),MAX_MSG_NUMS, MAX_MSG_LENTH));

			_process_ptr = std::make_shared<boost::process::child>(boost::process::child(
				boost::process::search_path("open-trade-ctp")
				,cmd.c_str()));
			if (nullptr == _process_ptr)
			{
				return false;
			}
			return _process_ptr->running();
		}
		else if (_reqLogin.broker.broker_type == "ctpse")
		{
			std::string cmd = "ctpse_" + _reqLogin.bid + "_" + _reqLogin.user_name;

			_out_mq_name = cmd + "_msg_out";
			_in_mq_name = cmd + "_msg_in";

			boost::interprocess::message_queue::remove(_out_mq_name.c_str());
			boost::interprocess::message_queue::remove(_in_mq_name.c_str());

			_out_mq_ptr = std::shared_ptr <boost::interprocess::message_queue>
				(new boost::interprocess::message_queue(boost::interprocess::create_only
					, _out_mq_name.c_str(), MAX_MSG_NUMS, MAX_MSG_LENTH));
			_thread_ptr.reset();

			_thread_ptr = std::shared_ptr<boost::thread>(
				new boost::thread(boost::bind(&UserProcessInfo::ReceiveMsg_i, this)));

			_in_mq_ptr = std::shared_ptr <boost::interprocess::message_queue>
				(new boost::interprocess::message_queue(boost::interprocess::create_only
					, _in_mq_name.c_str(), MAX_MSG_NUMS, MAX_MSG_LENTH));

			_process_ptr = std::make_shared<boost::process::child>(boost::process::child(
				boost::process::search_path("open-trade-ctpse")
				, cmd.c_str()));
			if (nullptr == _process_ptr)
			{
				return false;
			}
			return _process_ptr->running();
		}
		else if (_reqLogin.broker.broker_type == "sim")
		{
			std::string cmd = "sim_" + _reqLogin.bid + "_" + _reqLogin.user_name;
			
			_out_mq_name = cmd + "_msg_out";
			_in_mq_name = cmd + "_msg_in";

			boost::interprocess::message_queue::remove(_out_mq_name.c_str());
			boost::interprocess::message_queue::remove(_in_mq_name.c_str());

			_out_mq_ptr = std::shared_ptr <boost::interprocess::message_queue>
				(new boost::interprocess::message_queue(boost::interprocess::create_only
					, _out_mq_name.c_str(),MAX_MSG_NUMS, MAX_MSG_LENTH));

			_thread_ptr.reset();
			_thread_ptr = std::shared_ptr<boost::thread>(
				new boost::thread(boost::bind(&UserProcessInfo::ReceiveMsg_i, this)));

			_in_mq_ptr = std::shared_ptr <boost::interprocess::message_queue>
				(new boost::interprocess::message_queue(boost::interprocess::create_only
					, _in_mq_name.c_str(),MAX_MSG_NUMS,MAX_MSG_LENTH));

			_process_ptr = std::make_shared<boost::process::child>(boost::process::child(
				boost::process::search_path("open-trade-sim")
				,cmd.c_str()));
			if (nullptr == _process_ptr)
			{
				return false;
			}
			return _process_ptr->running();
		}
		else
		{
			Log(LOG_ERROR,NULL,"trade server req_login invalid broker_type=%s"
				, _reqLogin.broker.broker_type.c_str());			
			return false;
		}		
	}
	catch (const std::exception& ex)
	{
		Log(LOG_WARNING, NULL
			,"UserProcessInfo::StartProcess() fail:%s!",ex.what());
		return false;
	}	
}

void UserProcessInfo::StopProcess()
{
	if ((nullptr != _process_ptr)
		&&(_process_ptr->running()))
	{
		user_connections_.clear();
		_thread_ptr.reset();
		boost::interprocess::message_queue::remove(_out_mq_name.c_str());
		boost::interprocess::message_queue::remove(_in_mq_name.c_str());
		_process_ptr->terminate();
	}
}

void UserProcessInfo::SendMsg(int connid,const std::string& msg)
{	
	if (nullptr == _in_mq_ptr)
	{
		Log(LOG_WARNING, NULL, "UserProcessInfo::SendMsg,nullptr == _in_mq_ptr");
		return;
	}

	std::stringstream ss;
	ss << connid << "|" << msg;
	std::string str = ss.str();
	try
	{
		_in_mq_ptr->send(str.c_str(),str.length(), 0);
	}
	catch (std::exception& ex)
	{
		Log(LOG_ERROR, NULL
			, "UserProcessInfo::SendMsg Erro:%s,msg:%s,length:%d"
			, ex.what(), str.c_str(), str.length());
	}	
}

void UserProcessInfo::NotifyClose(int connid)
{
	if (nullptr == _in_mq_ptr)
	{
		Log(LOG_WARNING, NULL, "UserProcessInfo::NotifyClose,nullptr == _in_mq_ptr");
		return;
	}

	std::stringstream ss;
	ss << connid;
	std::string str = ss.str();
	try
	{
		_in_mq_ptr->send(str.c_str(),str.length(),0);
	}
	catch (std::exception& ex)
	{
		Log(LOG_ERROR, NULL
			, "UserProcessInfo::SendMsg Erro:%s,msg:%s,length:%d"
			, ex.what(), str.c_str(), str.length());
	}
}

void UserProcessInfo::ReceiveMsg_i()
{	
	std::string _str_packge_splited="";
	bool _packge_is_begin=false;
	char buf[MAX_MSG_LENTH];
	unsigned int priority;
	long long now1 = 0;
	long long now2 = 0;
	boost::interprocess::message_queue::size_type recvd_size;
	while (true)
	{
		try
		{
			memset(buf,0,sizeof(buf));
			_out_mq_ptr->receive(buf, sizeof(buf), recvd_size, priority);	
			std::string msg(buf);
			if (msg == BEGIN_OF_PACKAGE)
			{
				now1 =
					duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
				_str_packge_splited = "";
				_packge_is_begin = true;
				continue;
			}
			else if (msg == END_OF_PACKAGE)
			{
				_packge_is_begin = false;
				if (_str_packge_splited.length() > 0)
				{
					std::shared_ptr<std::string> msg_ptr =
						std::shared_ptr<std::string>(new std::string(_str_packge_splited));
					io_context_.post(boost::bind(&UserProcessInfo::ProcessMsg
						, this,msg_ptr));					
				}
				now2 =duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
				int ms = static_cast<int>(now2 - now1);
				Log(LOG_INFO, NULL, "UserProcessInfo::ReceiveMsg time:%d", ms);
				continue;
			}
			else
			{
				if (_packge_is_begin)
				{
					_str_packge_splited += msg;
					continue;
				}
				else
				{
					std::shared_ptr<std::string> msg_ptr =
						std::shared_ptr<std::string>(new std::string(msg));
					io_context_.post(boost::bind(&UserProcessInfo::ProcessMsg
						, this, msg_ptr));
					continue;
				}
			}			
		}
		catch (const std::exception& ex)
		{
			Log(LOG_ERROR,NULL,"ReceiveMsg_i Erro:%s",ex.what());
		}		
	}	
	boost::interprocess::message_queue::remove(_out_mq_name.c_str());
}

void UserProcessInfo::ProcessMsg(std::shared_ptr<std::string> msg_ptr)
{	
	if (nullptr == msg_ptr)
	{
		return;
	}

	std::string msg = *msg_ptr;	

	std::vector<std::string> items;
	boost::algorithm::split(items, msg, boost::algorithm::is_any_of("#"));
	//正常的数据
	if (2 == items.size())
	{
		std::string strIds = items[0];
		std::string strMsg = items[1];
		std::vector<std::string> ids;
		boost::algorithm::split(ids, strIds, boost::algorithm::is_any_of("|"));
		for (auto strId : ids)
		{
			int nId = atoi(strId.c_str());
			auto it = user_connections_.find(nId);
			if (it == user_connections_.end())
			{
				continue;
			}
			connection_ptr conn_ptr = it->second;
			if (nullptr != conn_ptr)
			{
				conn_ptr->SendTextMsg(strMsg);
			}
		}
	}
	else
	{
		Log(LOG_WARNING, NULL
			, "UserProcessInfo receive invalid message from trade instance:%s!"
			, msg.c_str());
		return;
	}
}
