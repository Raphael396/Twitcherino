#include "common/NetworkPrivate.hpp"

#include "common/NetworkManager.hpp"
#include "common/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "debug/Log.hpp"
#include "singletons/Paths.hpp"
#include "util/DebugCount.hpp"
#include "util/PostToThread.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QtConcurrent>

namespace chatterino {

NetworkData::NetworkData()
    : timer_(new QTimer())
{
    timer_->setSingleShot(true);

    DebugCount::increase("NetworkData");
}

NetworkData::~NetworkData()
{
    this->timer_->deleteLater();

    DebugCount::decrease("NetworkData");
}

QString NetworkData::getHash()
{
    if (this->hash_.isEmpty())
    {
        QByteArray bytes;

        bytes.append(this->request_.url().toString());

        for (const auto &header : this->request_.rawHeaderList())
        {
            bytes.append(header);
        }

        QByteArray hashBytes(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));

        this->hash_ = hashBytes.toHex();
    }

    return this->hash_;
}

void writeToCache(const std::shared_ptr<NetworkData> &data,
                  const QByteArray &bytes)
{
    if (data->useQuickLoadCache_)
    {
        QtConcurrent::run([data, bytes] {
            QFile cachedFile(getPaths()->cacheDirectory() + "/" +
                             data->getHash());

            if (cachedFile.open(QIODevice::WriteOnly))
            {
                cachedFile.write(bytes);
            }
        });
    }
}

void loadUncached(const std::shared_ptr<NetworkData> &data)
{
    DebugCount::increase("http request started");

    NetworkRequester requester;
    NetworkWorker *worker = new NetworkWorker;

    worker->moveToThread(&NetworkManager::workerThread);

    if (data->hasTimeout_)
    {
        data->timer_->setSingleShot(true);
        data->timer_->start();
    }

    auto onUrlRequested = [data, worker]() mutable {
        auto reply = [&]() -> QNetworkReply * {
            switch (data->requestType_)
            {
                case NetworkRequestType::Get:
                    return NetworkManager::accessManager.get(data->request_);

                case NetworkRequestType::Put:
                    return NetworkManager::accessManager.put(data->request_,
                                                             data->payload_);

                case NetworkRequestType::Delete:
                    return NetworkManager::accessManager.deleteResource(
                        data->request_);

                case NetworkRequestType::Post:
                    return NetworkManager::accessManager.post(data->request_,
                                                              data->payload_);
            }
        }();

        if (reply == nullptr)
        {
            log("Unhandled request type");
            return;
        }

        if (data->timer_->isActive())
        {
            QObject::connect(data->timer_, &QTimer::timeout, worker,
                             [reply, data]() {
                                 log("Aborted!");
                                 reply->abort();
                                 if (data->onError_)
                                 {
                                     data->onError_(-2);
                                 }
                             });
        }

        if (data->onReplyCreated_)
        {
            data->onReplyCreated_(reply);
        }

        auto handleReply = [data, reply]() mutable {
            if (data->hasCaller_ && !data->caller_.get())
            {
                return;
            }

            // TODO(pajlada): A reply was received, kill the timeout timer
            if (reply->error() != QNetworkReply::NetworkError::NoError)
            {
                if (data->onError_)
                {
                    data->onError_(reply->error());
                }
                return;
            }

            QByteArray bytes = reply->readAll();
            writeToCache(data, bytes);

            NetworkResult result(bytes);

            DebugCount::increase("http request success");
            // log("starting {}", data->request_.url().toString());
            if (data->onSuccess_)
            {
                if (data->executeConcurrently)
                    QtConcurrent::run(
                        [onSuccess = std::move(data->onSuccess_),
                         result = std::move(result)] { onSuccess(result); });
                else
                    data->onSuccess_(result);
            }
            // log("finished {}", data->request_.url().toString());

            reply->deleteLater();
        };

        QObject::connect(
            reply, &QNetworkReply::finished, worker,
            [data, handleReply, worker]() mutable {
                if (data->executeConcurrently || isGuiThread())
                {
                    handleReply();
                    delete worker;
                }
                else
                {
                    postToThread(
                        [worker, cb = std::move(handleReply)]() mutable {
                            cb();
                            delete worker;
                        });
                }
            });
    };

    QObject::connect(&requester, &NetworkRequester::requestUrl, worker,
                     onUrlRequested);

    emit requester.requestUrl();
}

// First tried to load cached, then uncached.
void loadCached(const std::shared_ptr<NetworkData> &data)
{
    QFile cachedFile(getPaths()->cacheDirectory() + "/" + data->getHash());

    if (!cachedFile.exists() || !cachedFile.open(QIODevice::ReadOnly))
    {
        // File didn't exist OR File could not be opened
        loadUncached(data);
        return;
    }
    else
    {
        // XXX: check if bytes is empty?
        QByteArray bytes = cachedFile.readAll();
        NetworkResult result(bytes);

        if (data->onSuccess_)
        {
            if (data->executeConcurrently || isGuiThread())
            {
                // XXX: If outcome is Failure, we should invalidate the cache file
                // somehow/somewhere
                /*auto outcome =*/
                if (data->hasCaller_ && !data->caller_.get())
                {
                    return;
                }
                data->onSuccess_(result);
            }
            else
            {
                postToThread([data, result]() {
                    if (data->hasCaller_ && !data->caller_.get())
                    {
                        return;
                    }

                    data->onSuccess_(result);
                });
            }
        }
    }
}

void load(const std::shared_ptr<NetworkData> &data)
{
    if (data->useQuickLoadCache_)
    {
        QtConcurrent::run(loadCached, data);
        loadCached(data);
    }
    else
    {
        loadUncached(data);
    }
}

}  // namespace chatterino