#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nonstd/optional.hpp>

#include "chainerx/backend.h"
#include "chainerx/device.h"
#include "chainerx/device_id.h"
#include "chainerx/graph.h"
#include "chainerx/macro.h"

namespace chainerx {
namespace native {

class NativeBackend;

}  // namespace native

namespace context_detail {

// Deleter for backend object whose memory may be managed by external module.
class BackendDeleter {
public:
    BackendDeleter()
        : destroy_backend_func_{[](Backend*) {
              XCHAINER_NEVER_REACH();  // Default-ctor is only for default-constructed unique_ptr which never calls deleter.
          }} {}
    explicit BackendDeleter(void (*destroy_backend_func)(Backend*)) : destroy_backend_func_{destroy_backend_func} {}

    void operator()(Backend* backend) { destroy_backend_func_(backend); }

private:
    void (*destroy_backend_func_)(Backend*);
};

}  // namespace context_detail

// TODO(sonots): Hide BackpropId-related functions from users.
// TODO(sonots): Move implementations of BackpropId-releated functions into another class.
// TODO(niboshi): Make BackpropId-related functions thread-safe.
class Context {
public:
    Context();
    ~Context();

    Context(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(const Context&) = delete;
    Context& operator=(Context&&) = delete;

    // Gets the backend specified by the name.
    // If the backend does not exist, this function automatically creates it.
    Backend& GetBackend(const std::string& backend_name);

    // Gets the native backend.
    native::NativeBackend& GetNativeBackend();

    // Gets the device specified by the device ID.
    // If the backend and/or device do not exist, this function automatically creates them.
    Device& GetDevice(const DeviceId& device_id);

    BackpropId MakeBackpropId(std::string backprop_name);

    void ReleaseBackpropId(const BackpropId& backprop_id);

    // TODO(sonots): Hide from users
    void ReleaseBackpropIdNoExcept(const BackpropId& backprop_id) noexcept;

    // Checks the specified backprop ID is valid, i.e. not released.
    void CheckValidBackpropId(const BackpropId& backprop_id) const;

    // Declares that the two backprop IDs co-exist in any portion of computation graph.
    // Backpropping on the backprop ID with the lower ordinal will prohibit future backprop on the other.
    // TODO(sonots): Hide from users
    void ConnectBackpropIds(const BackpropId& backprop_id1, const BackpropId& backprop_id2);

    // Return the name of the backprop.
    // XchainerError is thrown if the backprop ID is expired or non-existent in the context.
    // TODO(sonots): Hide from users
    std::string GetBackpropName(const BackpropId& backprop_id);

    // Checks if the backprop ID is allowed to be backpropped.
    // Backprop is allowed if the order of backprop IDs which have been backpropped is not reversed in any of the previous backprop scopes.
    // XchainerError is thrown if the check fails.
    // TODO(sonots): Hide from users
    void CheckBackpropAllowed(const BackpropId& backprop_id);

    // Flags the backprop ID that it has been backpropped.
    // TODO(sonots): Hide from users
    void SetBackpropDone(const BackpropId& backprop_id);

    // Returns all backprop IDs created after the queried graph.
    // In many cases, these are also the graphs created in inner scopes.
    // The queried graph is excluded from the returned container.
    // TODO(sonots): Hide from users
    std::vector<BackpropId> GetInnerBackpropIds(const BackpropId& backprop_id);

    BackpropId default_backprop_id() {
        // The first entry is always the default backprop ID.
        XCHAINER_ASSERT(!backprop_set_.empty());
        return BackpropId{*this, backprop_set_.front().ordinal};
    }

private:
    // TODO(niboshi): Support multi-thread usage
    struct BackpropSetItem {
        BackpropSetItem(BackpropOrdinal ordinal, std::string name) : ordinal{ordinal}, name{std::move(name)} {}

        BackpropOrdinal ordinal;
        std::string name;

        // If this member has a value, it indicates that this Backprop ID is prohibited for further backprop.
        // Its value is the backprop ID which caused the prohibition.
        nonstd::optional<BackpropOrdinal> prohibiting_ordinal{nonstd::nullopt};
    };

    // Finds the BackpropSetItem instance.
    const BackpropSetItem* GetBackpropSetItem(BackpropOrdinal ordinal) const {
        return GetBackpropSetItemImpl<const Context*, const BackpropSetItem*>(this, ordinal);
    }

    // Finds the BackpropSetItem instance.
    BackpropSetItem* GetBackpropSetItem(BackpropOrdinal ordinal) {
        return GetBackpropSetItemImpl<Context*, BackpropSetItem*>(this, ordinal);
    }

    template <typename ThisPtr, typename ReturnType>
    static ReturnType GetBackpropSetItemImpl(ThisPtr this_ptr, BackpropOrdinal ordinal);

    std::unordered_map<std::string, std::unique_ptr<Backend, context_detail::BackendDeleter>> backends_;
    std::vector<void*> dlopen_handles_;
    mutable std::mutex mutex_;

    BackpropOrdinal next_backprop_ordinal_{0};

    std::vector<BackpropSetItem> backprop_set_{};

    // List of pairs of connected backprop IDs.
    // The first ordinal is always less than the second, which means backpropping on the first will prohibit future backprop on the second.
    std::vector<std::pair<BackpropOrdinal, BackpropOrdinal>> backprop_connections_;
};

// Gets/sets the context that used by default when current context is not set.
Context& GetGlobalDefaultContext();
void SetGlobalDefaultContext(Context* context);

namespace internal {

Context* GetDefaultContextNoExcept() noexcept;

}  // namespace internal

// Gets thread local default context.
Context& GetDefaultContext();

// Sets thread local default context.
//
// The thread local default device is reset to null if given context is different with previous default context.
void SetDefaultContext(Context* context);

// Returns the specified device on the default context.
inline Device& GetDevice(const DeviceId& device_id) { return GetDefaultContext().GetDevice(device_id); }

// Returns the specified backend on the default context.
inline Backend& GetBackend(const std::string& backend_name) { return GetDefaultContext().GetBackend(backend_name); }

// Returns the native backend on the default context.
inline native::NativeBackend& GetNativeBackend() { return GetDefaultContext().GetNativeBackend(); }

// Scope object that switches the default context by RAII.
class ContextScope {
public:
    ContextScope() : orig_ctx_{internal::GetDefaultContextNoExcept()}, orig_device_{internal::GetDefaultDeviceNoExcept()}, exited_{false} {}
    explicit ContextScope(Context& context) : ContextScope{} { SetDefaultContext(&context); }

    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    ContextScope& operator=(ContextScope&& other) = delete;

    ContextScope(ContextScope&& other) : orig_ctx_{other.orig_ctx_}, orig_device_{other.orig_device_}, exited_{other.exited_} {
        other.exited_ = true;
    }

    ~ContextScope() { Exit(); }

    // Explicitly recovers the original context. It will invalidate the scope object so that dtor will do nothing.
    void Exit() {
        if (!exited_) {
            SetDefaultContext(orig_ctx_);
            SetDefaultDevice(orig_device_);
            exited_ = true;
        }
    }

private:
    Context* orig_ctx_;
    Device* orig_device_;
    bool exited_;
};

}  // namespace chainerx
