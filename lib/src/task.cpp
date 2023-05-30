﻿//
// Created by xSeung on 2023/4/21.
//
#include "task.h"
#include "filesystem"
#include "mutex"
#include "spdlog/spdlog.h"
#include "sstream"
#include "thread"

namespace mtft {
    using namespace asio;

    void ReadJsonFromBuf(streambuf& buf, Json::Value& json) {
        auto        size = buf.data().size();
        std::string str((const char*)buf.data().data(), size);
        json.clear();
        if (!Json::Reader().parse(str, json)) {
            throw std::exception("json解析失败");
        }
        buf.consume(size);
    }

    uint32_t WriteJsonToBuf(streambuf& buf, Json::Value& json) {
        auto str  = json.toStyledString();
        auto size = str.size() * sizeof(char);
        json.clear();
        std::memcpy(buf.prepare(size).data(), str.data(), size);
        buf.commit(size);
        return size;
    }

    Work::Work(int i) {
        mstop = false;
        mid   = i;
    }
    Work::~Work() {
        stop();
        cstop.notify_one();
    }
    int Work::getID() const {
        return mid;
    }
    void Work::stop() {
        mstop = true;
    }

    UpWork::UpWork(const ip::tcp::endpoint& remote, const FileReader::ptr& reader) : Work(reader->getID()) {
        mremote = remote;
        mReader = reader;
    }

    bool UpWork::uploadFunc(const socket_ptr& sck) {
        std::atomic_bool        exit{ false };
        std::thread             t;
        std::mutex              mtx;
        std::condition_variable cond;
        std::stringstream       tid;
        tid << std::this_thread::get_id();
        try {
            streambuf   buf;
            uint32_t    size;
            uint32_t    progress;
            Json::Value json;
            // 接收JSON文件
            size = sck->receive(buf.prepare(JSONSIZE));
            buf.commit(size);
            // 解析JSON, 更改相应配置
            ReadJsonFromBuf(buf, json);
            progress = json[PROGRESS].asInt();
            mReader->seek(progress);
            spdlog::info("upwork({:6}): 调整进度到{}", tid.str(), progress);
            // 发送文件
            t = std::thread([&] {
                while (!exit) {
                    std::unique_lock<std::mutex> _(mtx);
                    if (cond.wait_for(_, std::chrono::milliseconds(TIMEOUT)) == std::cv_status::timeout) {
                        spdlog::warn("upwork({:6}): 超时", tid.str());
                        sck->cancel();
                        return;
                    }
                }
            });
            while ((!mReader->finished() || buf.data().size() != 0) && !mstop) {
                size = mReader->read(buf.prepare(BUFFSIZE).data(), BUFFSIZE);
                buf.commit(size);
                size = sck->send(buf.data());
                cond.notify_one();
                buf.consume(size);
            }
        }
        catch (const std::exception& e) {
            spdlog::warn("upwork({:6}): {}", tid.str(), e.what());
            t.join();
            return false;
        }
        exit = true;
        cond.notify_one();
        t.join();
        return true;
    }

    void UpWork::Func() {
        error_code        ec;
        std::thread       t;
        std::stringstream tid;
        tid << std::this_thread::get_id();
        bool ret;
        while (!mstop) {
            ret      = false;
            auto sck = std::make_shared<ip::tcp::socket>(mioc);
            sck->connect(mremote, ec);
            if (ec) {
                spdlog::warn("upwork({:6})创建连接: {}", tid.str(), ec.message());
                std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECTTIME));
                continue;
            }
            t = std::thread([&, this] {
                std::mutex                   m;
                std::unique_lock<std::mutex> _(m);
                cstop.wait(_, [&, this] {
                    if (mstop || ret) {
                        if (!ret) {
                            sck->cancel();
                        }
                        return true;
                    }
                    return false;
                });
            });
            spdlog::info("upwork({:6})连接到: {}", tid.str(), mremote.address().to_string());
            auto b = uploadFunc(sck);
            ret    = true;
            cstop.notify_one();
            t.join();
            if (b) {
                streambuf buf;
                sck->receive(buf.prepare(BUFFSIZE));
                break;
            }
        }
        spdlog::info("upwork({:6}): 已完成数据发送", tid.str());
    }

    DownWork::DownWork(const FileWriter::ptr& fwriter) : Work(fwriter->getID()) {
        mFwriter = fwriter;
        medp     = ip::tcp::endpoint(ip::address_v4::any(), 0);
        macp     = std::make_shared<ip::tcp::acceptor>(mioc, medp);
        macp->listen();
    }

    bool DownWork::downloadFunc(const socket_ptr& sck) {
        std::atomic_bool        exit{ false };
        std::thread             t;
        std::mutex              mtx;
        std::condition_variable cond;
        std::stringstream       tid;
        tid << std::this_thread::get_id();
        try {
            streambuf   buf;
            Json::Value json;
            uint32_t    size;
            // 向json写入配置
            json[PROGRESS] = mFwriter->getProgress();
            // json配置写入缓存, 并发送
            WriteJsonToBuf(buf, json);
            write(*sck, buf);
            t = std::thread([&] {
                while (!exit) {
                    std::unique_lock<std::mutex> _(mtx);
                    if (cond.wait_for(_, std::chrono::milliseconds(TIMEOUT)) == std::cv_status::timeout) {
                        spdlog::warn("downwork({:6}): 超时", tid.str());
                        sck->cancel();
                        return;
                    }
                }
            });
            while (!mFwriter->finished() && !mstop) {
                size = sck->receive(buf.prepare(BUFFSIZE));
                cond.notify_one();
                buf.commit(size);
                size = mFwriter->write(buf.data().data(), size);
                buf.consume(size);
            }
        }
        catch (const std::exception& e) {
            spdlog::warn("downwork({:6}): {}", tid.str(), e.what());
            t.join();
            return false;
        }
        exit = true;
        cond.notify_one();
        t.join();
        return true;
    }

    void DownWork::Func() {
        error_code        ec;
        bool              ret{ false };
        std::stringstream tid;
        tid << std::this_thread::get_id();
        std::thread t;
        while (!mstop) {
            auto sck = std::make_shared<ip::tcp::socket>(macp->accept(ec));
            t        = std::thread([&, this] {
                std::mutex                   m;
                std::unique_lock<std::mutex> _(m);
                cstop.wait(_, [&, this] {
                    if (mstop || ret) {
                        if (mstop) {
                            sck->cancel();
                        }
                        return true;
                    }
                    return false;
                });
            });
            if (ec) {
                spdlog::warn("downwork({:6}): {}", tid.str(), ec.message());
                continue;
            }
            spdlog::info("downwork({:6})接收连接: {}", tid.str(), sck->remote_endpoint().address().to_string());
            auto b = downloadFunc(sck);
            ret    = true;
            cstop.notify_one();
            t.join();
            if (b) {
                sck->send(buffer(" "));
                break;
            }
        }
        mFwriter->close();
        spdlog::info("downwork({:6}): 已完成数据接收", tid.str());
    }

    int DownWork::GetPort() {
        return macp->local_endpoint().port();
    }
    FileWriter::ptr DownWork::getFw() {
        return mFwriter;
    }

    Task::Task(const std::vector<FileWriter::ptr>& vec, const std::string& fname) {
        fName = fname;
        type  = TaskType::Down;
        mWorks.reserve(vec.size());
        for (auto&& e : vec) {
            mWorks.emplace_back(std::make_shared<DownWork>(e));
        }
    }

    Task::Task(const std::vector<std::tuple<ip::tcp::endpoint, FileReader::ptr>>& vec, const std::string& fname) {
        fName = fname;
        type  = TaskType::Up;
        mWorks.reserve(vec.size());
        for (auto&& [e, r] : vec) {
            mWorks.emplace_back(std::make_shared<UpWork>(e, r));
        }
    }

    void Task::stop() {
        for (auto&& e : mWorks) {
            e->stop();
        }
    }

    bool Task::empty() {
        return mWorks.empty();
    }

    Work::ptr Task::getWork() {
        auto t = mWorks.back();
        mWorks.pop_back();
        return t;
    }
    /**
     * @brief 返回下载任务各进程监听的对应端口
     *
     * @return std::vector<std::tuple<int, int>> id, port
     */
    std::vector<std::tuple<int, int>> Task::getPorts() {
        if (type != TaskType::Down) {
            spdlog::error("{}:{}", __FILE__, __LINE__);
            abort();
        }
        std::vector<std::tuple<int, int>> vec;
        vec.reserve(mWorks.size());
        for (auto&& e : mWorks) {
            vec.emplace_back(e->getID(), dynamic_cast<DownWork*>(e.get())->GetPort());
        }
        return vec;
    }
    // 已被舍弃
    std::vector<std::string> Task::getVec() {
        if (type != TaskType::Down) {
            spdlog::error("{}:{}", __FILE__, __LINE__);
            abort();
        }
        std::vector<std::string> vec;
        vec.reserve(mWorks.size());
        for (auto&& e : mWorks) {
            vec.emplace_back(dynamic_cast<DownWork*>(e.get())->getFw()->getFname());
        }
        return vec;
    }
    //
    std::string Task::getName() {
        return fName;
    }
    TaskType Task::getType() {
        return type;
    }

    TaskPool::TaskPool() {
        mstop    = false;
        mCurrent = std::make_shared<Task>();
        for (int i = 0; i < THREAD_N * ParallelN; i++) {
            // 工作线程
            mThreads.emplace_back([this, i] {
                spdlog::info("工作线程({:3}): 准备就绪", i);
                std::string name;
                TaskType    type;
                Work::ptr   work;
                while (true) {
                    {
                        // 访问mCurrent临界资源
                        std::unique_lock<std::mutex> _(mtxC);
                        // 当前任务不为空或程序没停止时, 进入临界区
                        condw.wait(_, [this] { return mstop || !mCurrent->empty(); });
                        if (mstop) {
                            break;
                        }
                        spdlog::info("工作线程thread({:2}): 开始工作", i);
                        work = mCurrent->getWork();
                        name = mCurrent->getName();
                        type = mCurrent->getType();
                    }
                    // 通知其他工作线程
                    condw.notify_one();
                    // 通知调度线程
                    condd.notify_one();
                    work->Func();
                    std::unique_lock<std::mutex> _(mtxM);
                    M.at({ name, type })++;
                    // 通知合并线程
                    condh.notify_one();
                }
                spdlog::info("工作线程thread({:2})退出", i);
            });
        }
        // 调度线程
        mThreads.emplace_back([this] {
            spdlog::info("调度线程: 准备就绪");
            while (!mstop) {
                {
                    // 访问mCurrent临界资源
                    std::unique_lock<std::mutex> _(mtxC);
                    condd.wait(_, [this] { return mstop || mCurrent->empty(); });
                    if (mstop) {
                        break;
                    }
                    // 访问mTaskQueue临界资源
                    {
                        std::unique_lock<std::mutex> _ul(mtxQ);
                        condd.wait(_ul, [this] { return mstop || !mTaskQueue.empty(); });
                        if (mstop) {
                            break;
                        }
                        mCurrent = mTaskQueue.front();
                        mTaskQueue.pop();
                    }
                    // 通知工作线程
                    condw.notify_one();
                    // 通知文件合并线程
                    condh.notify_one();
                }
                spdlog::info("调度线程thread({:2}): 完成一次调度", THREAD_N);
            }
            spdlog::info("调度线程thread({:2})退出", THREAD_N);
        });
        // 文件合并线程
        mThreads.emplace_back([this] {
            spdlog::info("文件合并线程: 准备就绪");
            std::vector<std::string> vecerase;
            std::vector<std::string> vecmerge;
            while (!mstop) {
                {
                    // 访问M临界变量
                    std::unique_lock<std::mutex> _(mtxM);
                    condh.wait(_, [&, this] {
                        for (auto&& [tup, i] : M) {
                            auto&& [name, t] = tup;
                            if (i == THREAD_N) {
                                if (t == TaskType::Down) {
                                    vecmerge.push_back(name);
                                }
                                else {
                                    vecerase.push_back(name);
                                }
                            }
                        }
                        for (auto&& e : vecerase) {
                            M.erase({ e, TaskType::Up });
                        }
                        vecerase.clear();
                        return !vecmerge.empty() || mstop;
                    });
                }
                if (mstop) {
                    break;
                }
                for (auto&& e : vecmerge) {
                    spdlog::info("接收完成");
                    spdlog::info("开始合并: {}", e);
                    if (FileWriter::merge(e)) {
                        spdlog::info("合并完成");
                    }
                    std::filesystem::remove_all(fmt::format("{}{}", e, DIR));
                    spdlog::info("清理目录: {}{}", e, DIR);
                }
                std::unique_lock<std::mutex> ul(mtxM);
                for (auto&& n : vecmerge) {
                    M.erase({ n, TaskType::Down });
                }
                vecmerge.clear();
            }
            spdlog::info("文件合并线程退出");
        });
        spdlog::info("线程池初始化完成");
    }
    TaskPool::~TaskPool() {
        mstop = true;
        mCurrent->stop();
        condw.notify_all();
        condd.notify_one();
        condh.notify_one();
        for (auto&& e : mThreads) {
            e.join();
        }
    }

    bool TaskPool::isrepeat(const std::string& name) {
        std::unique_lock<std::mutex> _(mtxM);
        for (auto&& [tup, i] : M) {
            auto&& [n, t] = tup;
            if (name == n && t == TaskType::Up) {
                return true;
            }
        }
        return false;
    }

    void TaskPool::submit(const Task::ptr& task) {
        // 访问mTaskQueue临界资源
        {
            std::unique_lock<std::mutex> _(mtxQ);
            std::unique_lock<std::mutex> _l(mtxM);
            M.insert({ { task->getName(), task->getType() }, 0 });
            mTaskQueue.push(task);
        }
        condw.notify_one();
        condd.notify_one();
        condh.notify_one();
        spdlog::info("任务已提交");
    }
}  // namespace mtft
