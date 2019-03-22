/////////////////////////////////////////////////////////////////////////
///@file trade_base.h
///@brief	交易后台接口基类
///@copyright	上海信易信息科技股份有限公司 版权所有 
/////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <queue>
#include <map>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

using namespace std::chrono_literals;

#include "types.h"
#include "config.h"
#include "rapid_serialize.h"

//线程安全的FIFO队列
class StringChannel
{
public:
    bool empty() 
	{
        std::lock_guard<std::mutex> lck(m_mutex);
        return m_items.empty();
    }

    void push_back(const std::string& item) 
	{
        //向队列尾部加入一个元素
        std::lock_guard<std::mutex> lck(m_mutex);
        m_items.push_back(item);
        m_cv.notify_one();
    }

    bool try_pop_front(std::string* out_str) 
	{
        //尝试从队列头部提取一个元素, 如果队列为空则立即返回false
        std::lock_guard<std::mutex> lck(m_mutex);
        if (m_items.empty())
            return false;
        *out_str = m_items.front();
        m_items.pop_front();
        return true;
    }

    bool pop_front(std::string* out_str,
		const std::chrono::time_point<std::chrono::system_clock> dead_line)
	{
        //尝试从队列头部提取一个元素, 如果队列为空则阻塞等待到dead_line为止, 如果一直为空则返回false
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_cv.wait_until(lk, dead_line, [=] {return !m_items.empty(); })) {
            return false;
        }
        *out_str = m_items.front();
        m_items.pop_front();
        // 通知前完成手动锁定，以避免等待线程只再阻塞（细节见 notify_one ）
        lk.unlock();
        m_cv.notify_one();
        return true;
    }

    std::list<std::string> m_items;

    std::mutex m_mutex;

    std::condition_variable m_cv;
};

namespace trader_dll
{
	class TraderBase
	{
	public:
		TraderBase(std::function<void(const std::string&)> callback);
		virtual ~TraderBase();
		virtual void Start(const ReqLogin& req_login);
		virtual void Stop();
		virtual bool NeedReset() { return false; };

		//输入TraderBase的数据包队列
		StringChannel m_in_queue;

		//工作线程
		std::thread m_worker_thread;
		std::function<void(const std::string&)> m_send_callback;
		std::atomic_bool m_running; //需要工作线程运行
		
		
		//工作线程已完
		bool m_finished;

		ReqLogin m_req_login;
		//登录请求,保存以备断线重连时使用
	protected:
		void Run();
		virtual void OnInit() {};
		virtual void OnIdle() {};
		virtual void OnFinish() {};
		virtual void ProcessInput(const char* msg) = 0;
		void Output(const std::string& json);
		void OutputNotify(long notify_code, const std::string& ret_msg, const char* level = "INFO", const char* type = "MESSAGE");

		//业务信息
		std::string m_user_id; //交易账号
		User m_data;   //交易账户全信息
		std::mutex m_data_mtx; //m_data访问的mutex
		int m_notify_seq;
		int m_data_seq;
		Account& GetAccount(const std::string account_key);
		Position& GetPosition(const std::string position_key);
		Order& GetOrder(const std::string order_key);
		Trade& GetTrade(const std::string trade_key);
		Bank& GetBank(const std::string& bank_id);
		TransferLog& GetTransferLog(const std::string& seq_id);
		std::string m_user_file_path;
	};
}