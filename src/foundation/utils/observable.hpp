#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/threading/i_thread.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lgc {

inline constexpr std::string_view kObservableLog { "Observable" };

enum class ObserverPriority {
    High,
    Normal,
    Low,
};

/// Numeric rank for sorting / logging (higher notifies first when `preserveOrder` is false).
[[nodiscard]] constexpr int priorityRank(ObserverPriority p) noexcept
{
    switch (p) {
    case ObserverPriority::High:
        return 2;
    case ObserverPriority::Normal:
        return 1;
    case ObserverPriority::Low:
        return 0;
    }
    return 1;
}

struct NotificationOptions {
    std::function<void(const std::exception&, const std::string&)> errorHandler;
    /// When false (default), expired weak observer slots are removed under write lock before copying
    /// the list for this notify. Set true to skip that pass (shared lock only) on hot paths.
    bool skipWeakSweep = false;
    /// When true, callbacks run in registration / list order; when false, higher priority first.
    bool preserveOrder = false;
};

class ObserverToken {
public:
    ObserverToken()
        : id_(nextId.fetch_add(1, std::memory_order_relaxed))
    {
        if (id_ == 0) {
            id_ = nextId.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ObserverToken(const ObserverToken&) = delete;
    ObserverToken& operator=(const ObserverToken&) = delete;
    ObserverToken(ObserverToken&&) = default;
    ObserverToken& operator=(ObserverToken&&) = default;

    bool operator==(const ObserverToken& other) const { return id_ == other.id_; }

    [[nodiscard]] std::size_t id() const { return id_; }

private:
    std::size_t id_;
    static std::atomic<std::size_t> nextId;
};

inline std::atomic<std::size_t> ObserverToken::nextId { 1 };

/// Subject side of observer pattern. Optional per-observer `IThread*` (`executor` in slots) routes
/// each notification through that thread (`dispatchAsync` when not already on its executor);
/// `nullptr` runs the callback on the caller of `notifyObservers*`.
template <typename Observer>
class Observable {
public:
    using ObserverPtr = std::shared_ptr<Observer>;
    using ObserverWeakPtr = std::weak_ptr<Observer>;
    using CallbackType = std::function<void(const ObserverPtr&)>;
    using PredicateType = std::function<bool(const ObserverPtr&)>;

    explicit Observable(std::shared_ptr<ILogger> logger = Logger::defaultLogger())
        : logger_(std::move(logger))
    {
    }

    Observable(const Observable&) = delete;
    Observable& operator=(const Observable&) = delete;
    Observable(Observable&&) = delete;
    Observable& operator=(Observable&&) = delete;

    virtual ~Observable() { clearObservers(); }

    [[nodiscard]] ObserverToken createToken() { return ObserverToken {}; }

    void addObserver(const ObserverPtr& observer, IThread* deliveryThread,
        ObserverPriority priority = ObserverPriority::Normal, const ObserverToken* token = nullptr)
    {
        if (!observer) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!contains_(observer)) {
            StrongSlot sref { observer, deliveryThread, priority, token ? token->id() : 0 };
            insertByPriority_(sref);
        }
    }

    void addObserver(const ObserverPtr& observer, ObserverPriority priority = ObserverPriority::Normal,
        const ObserverToken* token = nullptr)
    {
        addObserver(observer, nullptr, priority, token);
    }

    void addWeakObserver(const ObserverPtr& observer, IThread* deliveryThread,
        ObserverPriority priority = ObserverPriority::Normal, const ObserverToken* token = nullptr)
    {
        if (!observer) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!contains_(observer)) {
            WeakSlot wref { observer, deliveryThread, priority, token ? token->id() : 0 };
            insertByPriority_(wref);
        }
    }

    void addWeakObserver(const ObserverPtr& observer, ObserverPriority priority = ObserverPriority::Normal,
        const ObserverToken* token = nullptr)
    {
        addWeakObserver(observer, nullptr, priority, token);
    }

    [[nodiscard]] bool removeObserverByToken(const ObserverToken& token)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const std::size_t sizeBefore = observers_.size();
        observers_.remove_if([&token](const std::any& val) {
            if (!val.has_value()) {
                return false;
            }
            std::size_t tokenId = 0;
            if (val.type() == typeid(WeakSlot)) {
                tokenId = std::any_cast<WeakSlot>(val).tokenId;
            } else if (val.type() == typeid(StrongSlot)) {
                tokenId = std::any_cast<StrongSlot>(val).tokenId;
            }
            return tokenId == token.id();
        });
        return sizeBefore > observers_.size();
    }

    void removeObserver(const ObserverPtr& observer)
    {
        if (!observer) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        observers_.remove_if([&observer](const std::any& val) { return slotMatches_(val, observer); });
    }

    void addObservers(const std::vector<ObserverPtr>& observers, IThread* deliveryThread = nullptr,
        ObserverPriority priority = ObserverPriority::Normal)
    {
        if (observers.empty()) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (const auto& observer : observers) {
            if (!observer || contains_(observer)) {
                continue;
            }
            StrongSlot sref { observer, deliveryThread, priority, 0 };
            insertByPriority_(sref);
        }
    }

    void addWeakObservers(const std::vector<ObserverPtr>& observers, IThread* deliveryThread = nullptr,
        ObserverPriority priority = ObserverPriority::Normal)
    {
        if (observers.empty()) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (const auto& observer : observers) {
            if (!observer || contains_(observer)) {
                continue;
            }
            WeakSlot wref { observer, deliveryThread, priority, 0 };
            insertByPriority_(wref);
        }
    }

    void clearObservers()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        observers_.clear();
    }

    [[nodiscard]] std::size_t observerCount() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return activeObserverCount_();
    }

    [[nodiscard]] bool hasObserver(const ObserverPtr& observer) const
    {
        if (!observer) {
            return false;
        }
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return contains_(observer);
    }

    [[nodiscard]] bool isEmpty() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return activeObserverCount_() == 0;
    }

    // Fills `out` with the current slot list under lock. When `skipWeakSweep` is false, takes an
    /// exclusive lock, removes expired weak slots, then copies; otherwise a shared lock and copy only.
    void copyObserversLocked_(std::vector<std::any>& out, bool skipWeakSweep)
    {
        if (!skipWeakSweep) {
            std::unique_lock<std::shared_mutex> wlock(mutex_);
            (void)eraseExpiredWeakSlots_();
            out.reserve(observers_.size());
            out.assign(observers_.begin(), observers_.end());
        } else {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            out.reserve(observers_.size());
            out.assign(observers_.begin(), observers_.end());
        }
    }

    virtual void notifyObservers(CallbackType callback, const NotificationOptions& options = {})
    {
        if (!callback) {
            assert(false && "Observer callback cannot be null");
            return;
        }
        std::vector<std::any> observersCopy;
        copyObserversLocked_(observersCopy, options.skipWeakSweep);
        notifySnapshot_(observersCopy, callback, nullptr, options);
    }

    virtual void notifyObserversIf(
        PredicateType predicate, CallbackType callback, const NotificationOptions& options = {})
    {
        if (!callback || !predicate) {
            assert(false && "Observer callback and predicate cannot be null");
            return;
        }
        std::vector<std::any> observersCopy;
        copyObserversLocked_(observersCopy, options.skipWeakSweep);
        notifySnapshot_(observersCopy, callback, predicate, options);
    }

    virtual void notifyObserversBatch(const std::vector<std::pair<PredicateType, CallbackType>>& notifications,
        const NotificationOptions& options = {})
    {
        if (notifications.empty()) {
            return;
        }
        for (const auto& [predicate, callback] : notifications) {
            if (!callback) {
                assert(false && "Observer callback cannot be null");
                return;
            }
        }
        std::vector<std::any> observersCopy;
        copyObserversLocked_(observersCopy, options.skipWeakSweep);
        for (const auto& [predicate, callback] : notifications) {
            notifySnapshot_(observersCopy, callback, predicate, options);
        }
    }

    void setDebug(bool enabled) { debug_ = enabled; }

    [[nodiscard]] bool debug() const { return debug_; }

    void setLogger(std::shared_ptr<ILogger> value) { logger_ = std::move(value); }

    [[nodiscard]] std::shared_ptr<ILogger> logger() const { return logger_; }

    [[nodiscard]] std::string describeObservers() const
    {
        std::ostringstream ss;
        std::size_t validCount = 0;
        std::size_t expiredCount = 0;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        ss << "Observers\n===========\n";
        for (const auto& ref : observers_) {
            if (!ref.has_value()) {
                continue;
            }
            if (ref.type() == typeid(WeakSlot)) {
                const auto wref = std::any_cast<WeakSlot>(ref);
                const auto observer = wref.observer.lock();
                if (observer) {
                    ss << "weak (live) ptr=" << observer.get() << " executor="
                       << static_cast<const void*>(wref.deliveryThread) << " priority=" << priorityRank(wref.priority)
                       << " token=" << wref.tokenId << '\n';
                    ++validCount;
                } else {
                    ss << "weak (expired) token=" << wref.tokenId << '\n';
                    ++expiredCount;
                }
            } else if (ref.type() == typeid(StrongSlot)) {
                const auto sref = std::any_cast<StrongSlot>(ref);
                ss << "strong ptr=" << sref.observer.get() << " executor="
                   << static_cast<const void*>(sref.deliveryThread) << " priority=" << priorityRank(sref.priority)
                   << " token=" << sref.tokenId << '\n';
                ++validCount;
            }
        }
        ss << "===========\nActive: " << validCount << ", expired weak slots: " << expiredCount << '\n';
        return ss.str();
    }

    [[nodiscard]] std::size_t sweepExpiredWeakObservers()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return eraseExpiredWeakSlots_();
    }

    void close()
    {
        clearObservers();
        logTrace_("Observable close: observers cleared");
    }

    [[nodiscard]] bool hasExpiredWeakObservers() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& ref : observers_) {
            if (ref.has_value() && ref.type() == typeid(WeakSlot)) {
                const auto wref = std::any_cast<WeakSlot>(ref);
                if (wref.observer.expired()) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] std::string statistics() const
    {
        const std::size_t n = observerCount();
        std::ostringstream ss;
        ss << "Observable observers=" << n << '\n';
        return ss.str();
    }

protected:
    [[nodiscard]] static bool slotMatches_(const std::any& val, const ObserverPtr& observer)
    {
        if (!val.has_value()) {
            return false;
        }
        if (val.type() == typeid(WeakSlot)) {
            const auto slot = std::any_cast<WeakSlot>(val);
            const auto locked = slot.observer.lock();
            return locked && locked == observer;
        }
        if (val.type() == typeid(StrongSlot)) {
            const auto slot = std::any_cast<StrongSlot>(val);
            return slot.observer == observer;
        }
        return false;
    }

    auto findEntry_(const ObserverPtr& observer) const
    {
        return std::find_if(observers_.cbegin(), observers_.cend(),
            [&observer](const std::any& val) { return slotMatches_(val, observer); });
    }

    [[nodiscard]] std::size_t activeObserverCount_() const
    {
        std::size_t count = 0;
        for (const auto& ref : observers_) {
            if (!ref.has_value()) {
                continue;
            }
            if (ref.type() == typeid(WeakSlot)) {
                const auto wref = std::any_cast<WeakSlot>(ref);
                if (!wref.observer.expired()) {
                    ++count;
                }
            } else {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] bool contains_(const ObserverPtr& observer) const
    {
        return findEntry_(observer) != observers_.cend();
    }

    template <typename RefType>
    void insertByPriority_(const RefType& ref)
    {
        if (observers_.empty() || ref.priority == ObserverPriority::Low) {
            observers_.emplace_back(ref);
            return;
        }
        for (auto it = observers_.begin(); it != observers_.end(); ++it) {
            ObserverPriority current = ObserverPriority::Normal;
            if (it->type() == typeid(WeakSlot)) {
                current = std::any_cast<WeakSlot>(*it).priority;
            } else if (it->type() == typeid(StrongSlot)) {
                current = std::any_cast<StrongSlot>(*it).priority;
            }
            if (priorityRank(ref.priority) > priorityRank(current)) {
                observers_.insert(it, ref);
                return;
            }
        }
        observers_.emplace_back(ref);
    }

    [[nodiscard]] std::size_t eraseExpiredWeakSlots_()
    {
        const std::size_t before = observers_.size();
        observers_.remove_if([](const std::any& val) {
            if (!val.has_value()) {
                return true;
            }
            if (val.type() == typeid(WeakSlot)) {
                return std::any_cast<WeakSlot>(val).observer.expired();
            }
            return false;
        });
        return before - observers_.size();
    }

    static void deliver_(IThread* deliveryThread, ObserverPtr observer, CallbackType callback,
        const std::function<void(const std::exception&, const std::string&)>& errorHandler)
    {
        const auto run = [observer, callback, errorHandler]() {
            try {
                callback(observer);
            } catch (const std::exception& ex) {
                std::ostringstream ctx;
                ctx << "callback observer=" << observer.get();
                errorHandler(ex, ctx.str());
            } catch (...) {
                try {
                    std::throw_with_nested(std::runtime_error("unknown callback error"));
                } catch (const std::exception& ex) {
                    errorHandler(ex, "unknown callback error");
                }
            }
        };

        if (deliveryThread == nullptr) {
            run();
            return;
        }
        if (deliveryThread->isCurrentThread()) {
            run();
            return;
        }
        deliveryThread->dispatchAsync([run]() { run(); });
    }

    void notifySnapshot_(const std::vector<std::any>& observers, CallbackType callback,
        PredicateType predicate, const NotificationOptions& options)
    {
        std::size_t notified = 0;
        auto errorHandler = options.errorHandler
            ? options.errorHandler
            : [this](const std::exception& ex, const std::string& context) {
                  if (debug_) {
                      try {
                          logTo(logger_,
                              LogLevel::Debug,
                              kObservableLog,
                              "notify error: {} ({})",
                              __FILE__,
                              __LINE__,
                              ex.what(),
                              context);
                      } catch (...) {
                      }
                  }
                  (void)ex;
                  (void)context;
              };

        std::vector<std::any> sorted = observers;
        if (!options.preserveOrder) {
            std::sort(sorted.begin(), sorted.end(), [](const std::any& a, const std::any& b) {
                ObserverPriority pa = ObserverPriority::Normal;
                ObserverPriority pb = ObserverPriority::Normal;
                if (a.has_value()) {
                    if (a.type() == typeid(WeakSlot)) {
                        pa = std::any_cast<WeakSlot>(a).priority;
                    } else if (a.type() == typeid(StrongSlot)) {
                        pa = std::any_cast<StrongSlot>(a).priority;
                    }
                }
                if (b.has_value()) {
                    if (b.type() == typeid(WeakSlot)) {
                        pb = std::any_cast<WeakSlot>(b).priority;
                    } else if (b.type() == typeid(StrongSlot)) {
                        pb = std::any_cast<StrongSlot>(b).priority;
                    }
                }
                return priorityRank(pa) > priorityRank(pb);
            });
        }

        for (const auto& ref : sorted) {
            if (!ref.has_value()) {
                continue;
            }
            ObserverPtr observer;
            IThread* deliveryThread = nullptr;
            try {
                if (ref.type() == typeid(WeakSlot)) {
                    const auto wref = std::any_cast<WeakSlot>(ref);
                    observer = wref.observer.lock();
                    deliveryThread = wref.deliveryThread;
                } else if (ref.type() == typeid(StrongSlot)) {
                    const auto sref = std::any_cast<StrongSlot>(ref);
                    observer = sref.observer;
                    deliveryThread = sref.deliveryThread;
                }
            } catch (const std::exception& ex) {
                errorHandler(ex, "type cast");
                continue;
            }
            if (!observer) {
                continue;
            }
            try {
                if (predicate && !predicate(observer)) {
                    continue;
                }
            } catch (const std::exception& ex) {
                std::ostringstream ctx;
                ctx << "predicate ptr=" << observer.get();
                errorHandler(ex, ctx.str());
                continue;
            }
            ++notified;
            deliver_(deliveryThread, observer, callback, errorHandler);
        }
        if (debug_ && notified > 0) {
            try {
                logTo(logger_,
                    LogLevel::Debug,
                    kObservableLog,
                    "notified {} observer(s)",
                    __FILE__,
                    __LINE__,
                    notified);
            } catch (...) {
            }
        }
    }

    void logTrace_(const std::string& message) const
    {
        if (!debug_) {
            return;
        }
        try {
            logTo(logger_, LogLevel::Debug, kObservableLog, "{}", __FILE__, __LINE__, message);
        } catch (...) {
        }
    }

private:
    struct StrongSlot {
        ObserverPtr observer;
        IThread* deliveryThread = nullptr;
        ObserverPriority priority = ObserverPriority::Normal;
        std::size_t tokenId = 0;
    };

    struct WeakSlot {
        ObserverWeakPtr observer;
        IThread* deliveryThread = nullptr;
        ObserverPriority priority = ObserverPriority::Normal;
        std::size_t tokenId = 0;
    };

    mutable std::shared_mutex mutex_;
    std::list<std::any> observers_;
    std::shared_ptr<ILogger> logger_;
    bool debug_ = false;
};

} // namespace lgc
