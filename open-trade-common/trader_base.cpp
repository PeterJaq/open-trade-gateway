/////////////////////////////////////////////////////////////////////////
///@file trade_base.cpp
///@brief	交易后台接口基类
///@copyright	上海信易信息科技股份有限公司 版权所有 
/////////////////////////////////////////////////////////////////////////

#include "trader_base.h"
#include "SerializerTradeBase.h"

namespace trader_dll
{

	TraderBase::TraderBase(std::function<void(const std::string&)> callback)
		: m_send_callback(callback)
	{
		m_running = true;
		m_finished = false;
		m_notify_seq = 0;
		m_data_seq = 0;
	}

	TraderBase::~TraderBase()
	{
	}

	void TraderBase::Output(const std::string& json)
	{
		if (m_running)
		{
			m_send_callback(json);
		}
	}


	using namespace std::chrono_literals;

	void TraderBase::Run()
	{
		OnInit();
		while (m_running)
		{
			std::string input_str;
			auto dead_line = std::chrono::system_clock::now() + 100ms;
			while (m_in_queue.pop_front(&input_str, dead_line))
			{
				ProcessInput(input_str.c_str());
			}
			OnIdle();
		}
		OnFinish();
		m_finished = true;
	}

	Account& TraderBase::GetAccount(const std::string account_key)
	{
		return m_data.m_accounts[account_key];
	}

	Position& TraderBase::GetPosition(const std::string symbol)
	{
		Position& position = m_data.m_positions[symbol];
		return position;
	}

	Order& TraderBase::GetOrder(const std::string order_id)
	{
		return m_data.m_orders[order_id];
	}

	Trade& TraderBase::GetTrade(const std::string trade_key)
	{
		return m_data.m_trades[trade_key];
	}

	Bank& TraderBase::GetBank(const std::string& bank_id)
	{
		return m_data.m_banks[bank_id];
	}

	TransferLog& TraderBase::GetTransferLog(const std::string& seq_id)
	{
		return m_data.m_transfers[seq_id];
	}

	void TraderBase::Start(const ReqLogin& req_login)
	{
		m_running = true;
		if (!g_config.user_file_path.empty())
			m_user_file_path = g_config.user_file_path + "/" + req_login.bid;
		m_req_login = req_login;
		m_user_id = m_req_login.user_name;
		m_data.user_id = m_user_id;
		m_worker_thread = std::thread(std::bind(&TraderBase::Run, this));
	}

	void TraderBase::Stop()
	{
		m_running = false;
	}

	void TraderBase::OutputNotify(long notify_code, const std::string& notify_msg, const char* level, const char* type)
	{
		//构建数据包
		SerializerTradeBase nss;
		rapidjson::Pointer("/aid").Set(*nss.m_doc, "rtn_data");
		// rapidjson::Value node_user_id;
		// node_user_id.SetString(m_user_id, nss.m_doc->GetAllocator());
		rapidjson::Value node_message;
		node_message.SetObject();
		node_message.AddMember("type", rapidjson::Value(type, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
		node_message.AddMember("level", rapidjson::Value(level, nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
		node_message.AddMember("code", notify_code, nss.m_doc->GetAllocator());
		node_message.AddMember("content", rapidjson::Value(notify_msg.c_str(), nss.m_doc->GetAllocator()).Move(), nss.m_doc->GetAllocator());
		rapidjson::Pointer("/data/0/notify/N" + std::to_string(m_notify_seq++)).Set(*nss.m_doc, node_message);
		std::string json_str;
		nss.ToString(&json_str);
		//发送
		Output(json_str);
	}

}