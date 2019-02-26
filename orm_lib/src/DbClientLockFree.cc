/**
 *
 *  DbClientLockFree.cc
 *  An Tao
 *
 *  Copyright 2018, An Tao.  All rights reserved.
 *  https://github.com/an-tao/drogon
 *  Use of this source code is governed by a MIT license
 *  that can be found in the License file.
 *
 *  Drogon
 *
 */

#include "DbClientLockFree.h"
#include "DbConnection.h"
#if USE_POSTGRESQL
#include "postgresql_impl/PgConnection.h"
#endif
#if USE_MYSQL
#include "mysql_impl/MysqlConnection.h"
#endif
#include "TransactionImpl.h"
#include <trantor/net/EventLoop.h>
#include <trantor/net/inner/Channel.h>
#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/DbClient.h>
#include <sys/select.h>
#include <iostream>
#include <thread>
#include <vector>
#include <unordered_set>
#include <memory>
#include <stdio.h>
#include <unistd.h>
#include <sstream>

using namespace drogon::orm;

DbClientLockFree::DbClientLockFree(const std::string &connInfo, trantor::EventLoop *loop, ClientType type)
    : _connInfo(connInfo),
      _loop(loop)
{
    _type = type;
    LOG_TRACE << "type=" << (int)type;
    if (type == ClientType::PostgreSQL)
    {
        _loop->runInLoop([this]() {
            for (size_t i = 0; i < _connectionNum; i++)
                _connectionHolders.push_back(newConnection());
        });
    }
    else if (type == ClientType::Mysql)
    {
        _loop->runInLoop([this]() {
            for (size_t i = 0; i < _connectionNum; i++)
                _connectionHolders.push_back(newConnection());
        });
    }
    else
    {
        LOG_ERROR << "No supported database type!";
    }
}

DbClientLockFree::~DbClientLockFree() noexcept
{
    for (auto &conn : _connections)
    {
        conn->disconnect();
    }
}

void DbClientLockFree::execSql(std::string &&sql,
                               size_t paraNum,
                               std::vector<const char *> &&parameters,
                               std::vector<int> &&length,
                               std::vector<int> &&format,
                               ResultCallback &&rcb,
                               std::function<void(const std::exception_ptr &)> &&exceptCallback)
{
    assert(paraNum == parameters.size());
    assert(paraNum == length.size());
    assert(paraNum == format.size());
    assert(rcb);
    _loop->assertInLoopThread();
    if (_connections.empty())
    {
        try
        {
            throw BrokenConnection("No connection to database server");
        }
        catch (...)
        {
            exceptCallback(std::current_exception());
        }
        return;
    }
    else
    {
        for (auto &conn : _connections)
        {
            if (!conn->isWorking())
            {
                conn->execSql(std::move(sql),
                              paraNum,
                              std::move(parameters),
                              std::move(length),
                              std::move(format),
                              std::move(rcb),
                              std::move(exceptCallback));
                return;
            }
        }
    }

    if (_sqlCmdBuffer.size() > 20000)
    {
        //too many queries in buffer;
        try
        {
            throw Failure("Too many queries in buffer");
        }
        catch (...)
        {
            exceptCallback(std::current_exception());
        }
        return;
    }

    //LOG_TRACE << "Push query to buffer";
    _sqlCmdBuffer.emplace_back(std::move(sql),
                               paraNum,
                               std::move(parameters),
                               std::move(length),
                               std::move(format),
                               std::move(rcb),
                               std::move(exceptCallback));
}

std::shared_ptr<Transaction> DbClientLockFree::newTransaction(const std::function<void(bool)> &commitCallback)
{
    // Don't support transaction;
    assert(0);
    return nullptr;
}

void DbClientLockFree::handleNewTask(const DbConnectionPtr &conn)
{
    assert(conn);
    assert(!conn->isWorking());
    if (!_sqlCmdBuffer.empty())
    {
        auto &cmd = _sqlCmdBuffer.front();
        conn->execSql(std::move(cmd._sql),
                      cmd._paraNum,
                      std::move(cmd._parameters),
                      std::move(cmd._length),
                      std::move(cmd._format),
                      std::move(cmd._cb),
                      std::move(cmd._exceptCb));
        _sqlCmdBuffer.pop_front();
        return;
    }
}

DbConnectionPtr DbClientLockFree::newConnection()
{
    DbConnectionPtr connPtr;
    if (_type == ClientType::PostgreSQL)
    {
#if USE_POSTGRESQL
        connPtr = std::make_shared<PgConnection>(_loop, _connInfo);
#else
        return nullptr;
#endif
    }
    else if (_type == ClientType::Mysql)
    {
#if USE_MYSQL
        connPtr = std::make_shared<MysqlConnection>(_loop, _connInfo);
#else
        return nullptr;
#endif
    }
    else
    {
        return nullptr;
    }

    std::weak_ptr<DbClientLockFree> weakPtr = shared_from_this();
    connPtr->setCloseCallback([weakPtr](const DbConnectionPtr &closeConnPtr) {
        //Erase the connection
        auto thisPtr = weakPtr.lock();
        if (!thisPtr)
            return;

        for (auto iter = thisPtr->_connections.begin(); iter != thisPtr->_connections.end(); iter++)
        {
            if (closeConnPtr == *iter)
            {
                thisPtr->_connections.erase(iter);
                break;
            }
        }
        for (auto iter = thisPtr->_connectionHolders.begin(); iter != thisPtr->_connectionHolders.end(); iter++)
        {
            if (closeConnPtr == *iter)
            {
                thisPtr->_connectionHolders.erase(iter);
                break;
            }
        }

        //Reconnect after 1 second
        thisPtr->_loop->runAfter(1, [weakPtr] {
            auto thisPtr = weakPtr.lock();
            if (!thisPtr)
                return;
            thisPtr->_connectionHolders.push_back(thisPtr->newConnection());
        });
    });
    connPtr->setOkCallback([weakPtr](const DbConnectionPtr &okConnPtr) {
        LOG_TRACE << "connected!";
        auto thisPtr = weakPtr.lock();
        if (!thisPtr)
            return;
        thisPtr->_connections.push_back(okConnPtr);
        thisPtr->handleNewTask(okConnPtr);
    });
    std::weak_ptr<DbConnection> weakConnPtr = connPtr;
    connPtr->setIdleCallback([weakPtr, weakConnPtr]() {
        auto thisPtr = weakPtr.lock();
        if (!thisPtr)
            return;
        auto connPtr = weakConnPtr.lock();
        assert(connPtr);
        thisPtr->handleNewTask(connPtr);
    });
    //std::cout<<"newConn end"<<connPtr<<std::endl;
    return connPtr;
}
