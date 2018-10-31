/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_HARDWARE_NEURALNETWORKS_V1_0_CALLBACKS_H
#define ANDROID_HARDWARE_NEURALNETWORKS_V1_0_CALLBACKS_H

#include <android/hardware/neuralnetworks/1.0/IExecutionCallback.h>
#include <android/hardware/neuralnetworks/1.0/IPreparedModelCallback.h>
#include <android/hardware/neuralnetworks/1.2/IExecutionCallback.h>
#include <android/hardware/neuralnetworks/1.2/IPreparedModelCallback.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace V1_2 {
namespace implementation {

using ::android::sp;
using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using V1_0::ErrorStatus;

/**
 * The CallbackBase class is used internally by the NeuralNetworks runtime to
 * synchronize between different threads. An asynchronous task is launched
 * paired with a callback object. When a client thread requires the output being
 * generated by the asynchronous task, the client thread can wait for the result
 * and be blocked until it has completed or a timeout condition has been
 * reached. Any wait* may safely be called concurrently, even on the same
 * callback object. When the asynchronous task has finished its workload, it
 * must immediately call "notify". If the asynchronous task has failed to launch,
 * the function that tried to launch the asynchronous task must immediately call
 * "notify". This "notify" call awakens any client threads waiting on the
 * callback object.
 *
 * The CallbackBase class implements some of the base synchronization common to
 * both PrepareModelCallback and ExecutionCallback. For consistency, any HIDL
 * callback class must inherit from CallbackBase as well as the HIDL callback
 * interface it implements.
 *
 * This class exists to enable synchronization across HIDL. When synchronization
 * is only required in the same process, consider using std::future, std::mutex,
 * std::condition_variable, or std::experimental::latch instead.
 */
class CallbackBase {
 public:
    CallbackBase();
    ~CallbackBase();

    /**
     * CallbackBase::wait blocks until notify has been called on the callback
     * object.
     */
    void wait();

    /**
     * CallbackBase::wait_for blocks until notify has been called on the
     * callback object or the time duration from the time the wait_for function
     * was called has expired, whichever comes first.
     *
     * @return Status std::cv_status::no_timeout if the callback was notified
     *                before the time duration expired, std::cv_status::timeout
     *                otherwise.
     */
    template<class Rep, class Period>
    std::cv_status wait_for(const std::chrono::duration<Rep,Period>& timeout_duration);

    /**
     * CallbackBase::on_finish binds a function to the callback object. This
     * bound function will be executed when CallbackBase::notify is called,
     * before any calls to wait* return. (Note that CallbackBase::wait_for can
     * return std::cv_status::timeout before CallbackBase::notify is called for
     * the first time, and hence before the bound function is executed.)
     *
     * The bound function must not synchronize with or otherwise access the
     * callback object it is bound to, as this could cause a deadlock.
     *
     * CallbackBase::on_finish can be called at most once on a given callback
     * object, and the call to CallbackBase::on_finish must finish before
     * CallbackBase::notify is called.
     *
     * @param post_work Function to be invoked the first time
     *                  CallbackBase::notify is called. Must have a target --
     *                  i.e., must not compare equal to nullptr. post_work
     *                  returns true if it successfully completes, false if it
     *                  fails.
     * @return bool True if the function was successfully bound, false if
     *              unsuccessful.
     *
     * TODO: Why does the return value of the callback matter?
     */
    bool on_finish(std::function<bool(void)> post_work);

    /**
     * CallbackBase::bind_thread binds a thread to the event for later use by
     * CallbackBase::join_thread.
     *
     * The thread must be passed using std::move.
     *
     * Once a thread is bound with CallbackBase::bind_thread, the client code
     * should ensure that one of the following occurs before the event is
     * destroyed:
     * - CallbackBase::join_thread has been called.
     * - CallbackBase::wait has been called.
     * - CallbackBase::wait_for has been called and returned other than
     *   std::cv_status::no_timeout.
     *
     * The bound thread shall not call any CallbackBase method with the
     * exception of CallbackBase::notify, which it must call when the thread has
     * finished its computation.
     *
     * CallbackBase::bind_thread can be called at most once on a given callback
     * object.
     *
     * @param asyncThread Thread to be bound to the callback object. The thread
     *                    object must represent a thread of execution -- i.e.,
     *                    asyncThread.joinable() must be true.
     * @return bool True if successful, false if thread was not properly bound.
     */
    bool bind_thread(std::thread&& asyncThread);

    /**
     * CallbackBase::join_thread ensures that the thread (if any) bound to this
     * event with CallbackBase::bind_thread has fully finished and cleaned its
     * resources. It is legal to call this function multiple times, concurrently
     * or sequentially.
     */
    void join_thread();

 protected:
    /**
     * CallbackBase::notify enables all prior and future wait* calls on the
     * callback object to proceed. The call to CallbackBase::notify happens
     * before any wait* calls on this callback object return (except in the case
     * of wait_for timing out). The asynchronous call the callback object is
     * paired with must ensure that any update to state that should be visible
     * to the caller of wait* happens before the call to CallbackBase::notify.
     *
     * CallbackBase::notify must be called exactly once on a given callback
     * object.
     */
    void notify();

 private:
    // Same as CallbackBase::join_thread but assumes we already hold a lock on
    // mMutex.
    void join_thread_locked();

    bool                      mNotified;
    std::mutex                mMutex;
    std::condition_variable   mCondition;
    std::function<bool(void)> mPostWork;
    std::thread               mThread;
};

/**
 * The PreparedModelCallback class is used to receive the error status of
 * preparing a model as well as the prepared model from a task executing
 * asynchronously with respect to the runtime. If a calling thread calls wait*
 * or get* on a PreparedModelCallback object and the corresponding asynchronous
 * task has not finished preparing the model, the calling thread will block
 * until the asynchronous task has either called notify or notify_1_2. For more
 * information on the synchronization behavior, refer to the CallbackBase class.
 *
 * This class inherits the basic blocking and signaling calls from
 * CallbackBase, and implements the HIDL notify and notify_1_2 calls from
 * IPreparedModelCallback. This callback object is passed as an argument to
 * IDevice::prepareModel.
 */
class PreparedModelCallback : public CallbackBase, public IPreparedModelCallback {
 public:
    PreparedModelCallback();
    ~PreparedModelCallback() override;

    /**
     * IPreparedModelCallback::notify and IPreparedModelCallback::notify_1_2
     * mark the callback object with the return status of the asynchronous
     * model preparation along with the prepared model, and call
     * CallbackBase::notify, enabling all prior and future wait* calls on the
     * PreparedModelCallback object to proceed. For more information on the
     * synchronization behavior, refer to the CallbackBase class.
     *
     * Either IPreparedModelCallback::notify or IPreparedModelCallback::notify_1_2
     * must be called exactly once on a given PreparedModelCallback object.
     *
     * @param status Error status returned from asynchronously preparing the
     *               model; will be:
     *               - NONE if the asynchronous preparation was successful
     *               - DEVICE_UNAVAILABLE if driver is offline or busy
     *               - GENERAL_FAILURE if there is an unspecified error
     *               - INVALID_ARGUMENT if the input model is invalid
     * @param preparedModel Returned model that has been prepared for execution,
     *                      nullptr if the model was unable to be prepared.
     */
    Return<void> notify(ErrorStatus status, const sp<V1_0::IPreparedModel>& preparedModel) override;
    Return<void> notify_1_2(ErrorStatus status,
                            const sp<V1_2::IPreparedModel>& preparedModel) override;

    /**
     * Retrieves the error status returned from the asynchronous task launched
     * by IDevice::prepareModel. If IDevice::prepareModel has not finished
     * asynchronously preparing the model, this call will block until the
     * asynchronous task notifies the object.
     *
     * @return status Error status returned from asynchronously preparing the
     *                model; will be:
     *                - NONE if the asynchronous preparation was successful
     *                - DEVICE_UNAVAILABLE if driver is offline or busy
     *                - GENERAL_FAILURE if there is an unspecified error
     *                - INVALID_ARGUMENT if the input model is invalid
     */
    ErrorStatus getStatus();

    /**
     * Retrieves the model that has been prepared for execution from the
     * asynchronous task launched by IDevice::prepareModel. If
     * IDevice::prepareModel has not finished asynchronously preparing the
     * model, this call will block until the asynchronous task notifies the
     * object.
     *
     * @return preparedModel Returned model that has been prepared for
     *                       execution, nullptr if the model was unable to be
     *                       prepared.
     */
    sp<V1_0::IPreparedModel> getPreparedModel();

   private:
    ErrorStatus        mErrorStatus;
    sp<V1_0::IPreparedModel> mPreparedModel;
};

/**
 * The ExecutionCallback class is used to receive the error status of the
 * execution from a task executing asynchronously with respect to the runtime.
 * If a calling thread calls wait* or get* on a PreparedModelCallback object and
 * the corresponding asynchronous task has not finished the execution, the
 * calling thread will block until the asynchronous task has either called notify
 * or notify_1_2. For more information on the synchronization behavior, refer to
 * the CallbackBase class.
 *
 * This class inherits the basic blocking and signaling calls from
 * CallbackBase, and implements the HIDL notify and notify_1_2 calls from
 * IExecutionCallback. This callback object is passed as an argument to
 * IPreparedModel::execute.
 */
class ExecutionCallback : public CallbackBase,  public IExecutionCallback {
    using ExecutionFinish = std::function<ErrorStatus(ErrorStatus)>;

   public:
    ExecutionCallback();
    ~ExecutionCallback() override;

    /**
     * IExecutionCallback::notify and IExecutionCallback::notify_1_2 mark the
     * callback object with the return status of the asynchronous execution that
     * held this callback and enable all prior and future wait* calls on the
     * ExecutionCallback object to proceed. For more information on the
     * synchronization behavior, refer to the CallbackBase class.
     *
     * Either IExecutionCallback::notify or IExecutionCallback::notify_1_2 must
     * be called exactly once on a given ExecutionCallback object.
     *
     * @param status Error status returned from launching the asynchronous task
     *               (if the launch fails) or from the asynchronous task itself
     *               (if the launch succeeds). Must be:
     *               - NONE if the asynchronous execution was successful
     *               - DEVICE_UNAVAILABLE if driver is offline or busy
     *               - GENERAL_FAILURE if there is an unspecified error
     *               - OUTPUT_INSUFFICIENT_SIZE if provided output buffer is
     *                 not large enough to store the resultant values
     *               - INVALID_ARGUMENT if the input request is invalid
     */
    Return<void> notify(ErrorStatus status) override;

    /**
     * Similar to IExecutionCallback::notify, but for V1_2::IPreparedModel to
     * also notify output shapes along with error status.
     *
     * @param status Error status returned from launching the asynchronous task
     *               (if the launch fails) or from the asynchronous task itself
     *               (if the launch succeeds). Must be:
     *               - NONE if the asynchronous execution was successful
     *               - DEVICE_UNAVAILABLE if driver is offline or busy
     *               - GENERAL_FAILURE if the asynchronous task resulted in an
     *                 unspecified error
     *               - OUTPUT_INSUFFICIENT_SIZE if at least one output
     *                 operand buffer is not large enough to store the
     *                 corresponding output
     *               - INVALID_ARGUMENT if one of the input arguments to
     *                 prepareModel is invalid
     * @param outputShapes A list of shape information of model output operands.
     *                     The index into "outputShapes" corresponds to the index
     *                     of the output operand in the Request outputs vector.
     *                     outputShapes must be empty unless the status is either
     *                     NONE or OUTPUT_INSUFFICIENT_SIZE.
     */
    Return<void> notify_1_2(ErrorStatus status, const hidl_vec<OutputShape>& outputShapes) override;

    // An overload of the latest notify interface to hide the version from ExecutionBuilder.
    Return<void> notify(ErrorStatus status, const hidl_vec<OutputShape>& outputShapes) {
        return notify_1_2(status, outputShapes);
    }

    /**
     * Retrieves the error status returned from the asynchronous task launched
     * by either IPreparedModel::execute or IPreparedModel::execute_1_2. If
     * IPreparedModel::execute or IPreparedModel::execute_1_2 has not finished
     * asynchronously executing, this call will block until the asynchronous task
     * notifies the object.
     *
     * @return status Error status returned from launching the asynchronous task
     *                (if the launch fails) or from the asynchronous task itself
     *                (if the launch succeeds). Must be:
     *                - NONE if the asynchronous execution was successful
     *                - DEVICE_UNAVAILABLE if driver is offline or busy
     *                - GENERAL_FAILURE if the asynchronous task resulted in an
     *                  unspecified error
     *                - OUTPUT_INSUFFICIENT_SIZE if at least one output
     *                  operand buffer is not large enough to store the
     *                  corresponding output
     *                - INVALID_ARGUMENT if one of the input arguments to
     *                  prepareModel is invalid
     */
    ErrorStatus getStatus();

    /**
     * Retrieves the output shapes returned from the asynchronous task launched
     * by IPreparedModel::execute_1_2. If IPreparedModel::execute_1_2 has not finished
     * asynchronously executing, this call will block until the asynchronous task
     * notifies the object.
     *
     * If the asynchronous task was launched by IPreparedModel::execute, an empty vector
     * will be returned.
     *
     * @return outputShapes A list of shape information of model output operands.
     *                      The index into "outputShapes" corresponds to the index
     *                      of the output operand in the Request outputs vector.
     *                      outputShapes must be empty unless the status is either
     *                      NONE or OUTPUT_INSUFFICIENT_SIZE.
     */
    const std::vector<OutputShape>& getOutputShapes();

    // The callback will invoke finish(mErrorStatus) on finish.
    void setOnFinish(const ExecutionFinish& finish) { mOnFinish = finish; }

   private:
    ErrorStatus mErrorStatus;
    std::vector<OutputShape> mOutputShapes;
    ExecutionFinish mOnFinish;
};


// template function implementation(s) below this point

template<class Rep, class Period>
std::cv_status CallbackBase::wait_for(const std::chrono::duration<Rep,Period>& timeout_duration) {
    std::unique_lock<std::mutex> lock(mMutex);
    std::cv_status status = mCondition.wait_for(lock, timeout_duration, [this]{return mNotified;});
    if (status != std::cv_status::timeout) {
        join_thread_locked();
    }
    return status;
}

}  // namespace implementation
}  // namespace V1_2
}  // namespace neuralnetworks
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_NEURALNETWORKS_V1_0_CALLBACKS_H
