/////////////////////////////////////////////////////////////////////////
///@file user_process_info.h
///@brief	用户进程管理类
///@copyright	上海信易信息科技股份有限公司 版权所有 
/////////////////////////////////////////////////////////////////////////

#pragma once

#include "types.h"
#include "connection.h"

#include <boost/process.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/regex.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>

struct UserProcessInfo
{
	UserProcessInfo(boost::asio::io_context& ios
		, const ReqLogin& reqLogin);

	bool ProcessIsRunning();

	bool StartProcess();

	void StopProcess();

	void SendMsg(int connid, const std::string& msg);

	void NotifyClose(int connid);

	void ReceiveMsg_i();

	void ProcessMsg(std::shared_ptr<std::string> msg_ptr);

	boost::asio::io_context& io_context_;

	std::shared_ptr<boost::interprocess::message_queue> _out_mq_ptr;

	std::string _out_mq_name;

	std::shared_ptr<boost::thread> _thread_ptr;

	std::shared_ptr<boost::interprocess::message_queue> _in_mq_ptr;

	std::string _in_mq_name;

	std::shared_ptr<boost::process::child> _process_ptr;

	std::map<int, connection_ptr> user_connections_;

	ReqLogin _reqLogin;

	std::string _str_packge_splited;
};

typedef std::shared_ptr<UserProcessInfo> UserProcessInfo_ptr;

typedef std::map<std::string,UserProcessInfo_ptr> TUserProcessInfoMap;

extern TUserProcessInfoMap g_userProcessInfoMap;
