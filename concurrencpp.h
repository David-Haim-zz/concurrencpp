/*
	Copyright (c) 2017 - 2018 David Haim

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.
*/

#ifndef CONCURRENCPP_H
#define CONCURRENCPP_H

#include <vector>
#include <chrono>
#include <cassert>
#include <type_traits>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <experimental\coroutine>

#if defined(_WIN32)
#define CRCPP_WIN_OS
#endif

#if defined(unix) || defined(__unix__) || defined(__unix)
#define CRCPP_UNIX_OS
#endif

namespace concurrencpp {
	struct empty_object_exception : public std::runtime_error {
		empty_object_exception(const char* message) : runtime_error(message){}
	};
}

namespace concurrencpp {
	namespace details {
		class spinlock {

		private:
			std::atomic_flag locked = ATOMIC_FLAG_INIT;

			static inline void pause_cpu() noexcept {
#if (COMPILER == MVCC)
				_mm_pause();
#elif (COMPILER == GCC || COMPILER == LLVM)
				asm volatile("pause\n": : : "memory");		
#endif
			}

			static inline void sleep() noexcept { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

			bool try_lock_once(void (&on_fail_callable) ()) noexcept;

		public:
			void lock() noexcept;
			inline void unlock() noexcept { locked.clear(); }
			inline bool try_lock() noexcept { return locked.test_and_set(std::memory_order_acquire) == false; }
		};

		class recursive_spinlock {

		private:
			spinlock m_lock;
			std::atomic<std::thread::id> m_owner;
			intmax_t m_count;

		public:
			inline recursive_spinlock() noexcept : m_count(0) {}
			void lock() noexcept;
			void unlock() noexcept;
		};

		void* allocate(size_t size) noexcept;
		void deallocate(void* pointer, size_t size) noexcept;

		template <class type>
		struct pool_allocator {

			typedef type value_type;

			pool_allocator() noexcept {}

			template<class other_type>
			pool_allocator(const pool_allocator<other_type>&) noexcept {}

			template<class other_type>
			bool operator == (const pool_allocator<other_type>&) const noexcept { return true; }

			template<class other_type>
			bool operator != (const pool_allocator<other_type>&) const noexcept { return false; }

			type* allocate(const size_t count) const {
				type* const result = static_cast<type*>(::concurrencpp::details::allocate(count * sizeof(type)));
				if (result != nullptr) {
					return result;
				}

				throw std::bad_alloc();
			}

			void deallocate(type* const block, const size_t count) const noexcept {
				::concurrencpp::details::deallocate(block, count * sizeof(type));
			}
		};

		struct pool_allocated {
			static inline void* operator new(const size_t size) { return allocate(size); }
			static inline void operator delete(void* block, size_t size) { deallocate(block, size); }
		};

		template<class type>
		struct crcpp_deleter {	
			void operator() (type* ptr) const noexcept { 
				ptr->~type(); 
				deallocate(ptr, sizeof(type)); 
			}
		};

		template<class type, class ... argument_types>
		std::shared_ptr<type> make_shared(argument_types&& ... args) {
			pool_allocator<type> allocator;
			return std::allocate_shared<type>(allocator, std::forward<argument_types>(args)...);
		}
	}

	template<class type> class future;
	template<class type> struct promise;

	namespace details {

		template<class type> class future_associated_state;
		template<class type> struct unsafe_promise;

		struct callback_base {
			virtual ~callback_base() = default;
			virtual void execute() = 0;

			std::unique_ptr<callback_base> next;
		};

		template<class function_type>
		struct callback : public callback_base, public pool_allocated {

			using decayed_function_type = typename std::decay_t<function_type>;

		private:
			decayed_function_type m_function;

		public:
			callback(function_type&& function) : m_function(std::forward<function_type>(function)) {}
			virtual void execute() override final { m_function(); }
		};

		template<class function_type>
		std::unique_ptr<callback_base> make_callback(function_type&& function) {
			return std::unique_ptr<callback_base>(new callback<function_type>(std::forward<function_type>(function)));
		}

		class worker_thread;

		class thread_pool {

		private:
			std::atomic_bool m_stop;
			std::vector<worker_thread> m_workers;
			std::mutex m_pool_lock;

			size_t choose_next_worker() noexcept;
			worker_thread* get_self_worker_impl() noexcept;
			worker_thread* get_self_worker() noexcept;
			
			static size_t number_of_threads_cpu() noexcept;
			static size_t number_of_threads_io() noexcept;

		public:
			thread_pool(const size_t number_of_workers);
			~thread_pool() noexcept;

			void wait_for_pool_construction();
			bool is_working() noexcept;

			template<class function_type, class ... arguments>
			void enqueue_task(function_type&& function, arguments&& ... args) {
				enqueue_task(std::bind(std::forward<function_type>(function), std::forward<arguments>(args)...));
			}

			template<class function_type>
			void enqueue_task(function_type&& function) {
				enqueue_task(make_callback(std::forward<function_type>(function)));
			}

			std::unique_ptr<callback_base> dequeue_task() noexcept; //should be use by worker_thread_only!
			void enqueue_task(std::unique_ptr<callback_base> function) noexcept;
			static thread_pool& default_instance();
			static thread_pool& blocking_tasks_instance();
		};

		template<class type, class function_type>
		class future_then final : public callback_base, public pool_allocated {

			using new_type = typename std::result_of_t<function_type(::concurrencpp::future<type>)>;
			using decayed_function_type = std::decay_t<function_type>;

		private:
			::concurrencpp::future<type> m_future;
			unsafe_promise<new_type> m_promise;
			decayed_function_type m_function;

		public:
			inline future_then(::concurrencpp::future<type> future, function_type&& function) :
				m_future(std::move(future)),
				m_function(std::forward<function_type>(function)) {}

			inline void execute() {
				m_promise.set_from_function([this]() -> decltype(auto) {
					return m_function(std::move(m_future));
				});
			}

			inline ::concurrencpp::future<new_type> get_future() noexcept { return m_promise.get_future(); }
		};

		template<class type, class function_type>
		class deffered_future_task final : public callback_base, public pool_allocated {

			using decayed_function_type = std::decay_t<function_type>;

		private:
			::std::weak_ptr<future_associated_state<type>> m_state;
			decayed_function_type m_function;

		public:
			inline deffered_future_task(decltype(m_state) state, function_type&& function) :
				m_state(std::move(state)),
				m_function(std::forward<function_type>(function)) {}

			inline void execute() override {
				auto state = m_state.lock();
				assert(static_cast<bool>(state));
				set_future_state_from_function(*state, std::forward<decayed_function_type>(m_function));
			}
		};

		enum class future_result_status {
			NOT_READY,
			RESULT,
			EXCEPTION,
			DEFFERED
		};

		template<class type>
		union compressed_future_result {
			type result;
			std::exception_ptr exception;

			inline compressed_future_result() noexcept {};
			inline ~compressed_future_result() noexcept {};

			void destroy(future_result_status status) {
				if (status == future_result_status::RESULT) {
					result.~type();
				}
				if (status == future_result_status::EXCEPTION) {
					exception.~exception_ptr();
				}
			}

			type result_or_exception(future_result_status status) {
				if (status == future_result_status::RESULT) {
					return std::move(result);
				}

				assert(status == future_result_status::EXCEPTION);
				std::rethrow_exception(exception);
			}

			template <class ... argument_types>
			void set_result(argument_types&& ... args) { new (std::addressof(result)) type(std::forward<argument_types>(args)...); }
			inline void set_exception(std::exception_ptr exception_ptr) { new (std::addressof(exception)) std::exception_ptr(std::move(exception_ptr)); }
		};

		template<class type>
		union compressed_future_result<type&> {
			type* result;
			std::exception_ptr exception;

			inline compressed_future_result() noexcept {};
			inline ~compressed_future_result() noexcept {};

			void destroy(future_result_status status) {
				if (status == future_result_status::EXCEPTION) {
					exception.~exception_ptr();
				}
			}

			type& result_or_exception(future_result_status status) {
				if (status == future_result_status::RESULT) {
					return *result;
				}

				assert(status == future_result_status::EXCEPTION);
				std::rethrow_exception(exception);
			}

			inline void set_result(type& ref_result) noexcept { result = std::addressof(ref_result); }
			inline void set_exception(std::exception_ptr exception_ptr) { new (std::addressof(exception)) std::exception_ptr(std::move(exception_ptr)); }
		};

		template<>
		union compressed_future_result<void> {
			std::exception_ptr exception;

			inline compressed_future_result() noexcept {};
			inline ~compressed_future_result() noexcept {};

			void destroy(future_result_status status) {
				if (status == future_result_status::EXCEPTION) {
					exception.~exception_ptr();
				}
			}

			void result_or_exception(future_result_status status) {
				if (status == future_result_status::RESULT) {
					return;
				}

				assert(status == future_result_status::EXCEPTION);		
				std::rethrow_exception(exception);
			}

			inline void set_result() const noexcept {}
			inline void set_exception(std::exception_ptr exception_ptr) { new (std::addressof(exception)) std::exception_ptr(std::move(exception_ptr)); }
		};

		struct continuation_scheduler {
			void (&schedule_coro)(std::experimental::coroutine_handle<void>);
			void (&schedule_cb)(std::unique_ptr<callback_base>);

			inline continuation_scheduler(decltype(schedule_coro) schedule_coro, decltype(schedule_cb) schedule_cb) noexcept:
				schedule_coro(schedule_coro),
				schedule_cb(schedule_cb){}

			static void schedule_coro_threadpool(std::experimental::coroutine_handle<void>);
			static void schedule_cb_threadpool(std::unique_ptr<callback_base>);

			static continuation_scheduler thread_pool_scheduler;
		};

		class future_associated_state_base {
	
		protected:
			//members are ordered in the order of their importance.
			mutable recursive_spinlock m_lock;
			future_result_status m_status;
			::std::experimental::coroutine_handle<void> m_coro_handle;
			::std::unique_ptr<callback_base> m_then, m_deffered;
			mutable continuation_scheduler* m_scheduler; //never an owner!! we shouldn't care about RAII
			mutable std::unique_ptr<std::condition_variable_any, crcpp_deleter<std::condition_variable_any>> m_condition; //lazy creation

			void build_condition_object() const;
			std::condition_variable_any& get_condition_object();
			const std::condition_variable_any& get_condition_object() const;

			void execute_continuation_inline();
			void schedule_continuation();

		public:
			inline future_associated_state_base() noexcept : 
				m_status(future_result_status::NOT_READY),
				m_scheduler(nullptr){}

			template<class duration_unit, class ratio>
			::std::future_status wait_for(std::chrono::duration<duration_unit, ratio> duration) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);

				if (m_status == future_result_status::DEFFERED) {
					return ::std::future_status::deferred;
				}

				if (m_status != future_result_status::NOT_READY) {
					return ::std::future_status::ready;
				}

				auto& condition = get_condition_object();
				const auto has_result = condition.wait_for(lock, duration, [this] {
					return !(m_status == future_result_status::NOT_READY);
				});

				return has_result ? std::future_status::ready : std::future_status::timeout;
			}

			void wait();

			inline bool is_deffered_future() const noexcept { return m_status == future_result_status::DEFFERED; }
			inline void set_scheduler(continuation_scheduler& scheduler) noexcept { m_scheduler = &scheduler; }
			
			inline bool is_ready() const noexcept {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return m_status == future_result_status::EXCEPTION || m_status == future_result_status::RESULT;
			}

			void set_deffered_task(std::unique_ptr<callback_base> task) noexcept;
			void set_coro_handle(std::experimental::coroutine_handle<void> coro_handle) noexcept;
			void set_exception(std::exception_ptr exception_pointer, std::exception_ptr* exception_storage);
			void call_continuation();

			template<class return_type, class type, class function_type>
			::concurrencpp::future<return_type> set_then_not_ready(::concurrencpp::future<type> future, function_type&& function) {
				auto then = std::make_unique<future_then<type, function_type>>(std::move(future), std::forward<function_type>(function));
				auto ret_future = then->get_future();
				m_then.reset(then.release());
				return ret_future;
			}

			template<class return_type, class type, class function_type>
			::concurrencpp::future<return_type> set_then_deffered(::concurrencpp::future<type> future, function_type&& function) {
				//instead of dragging the entire concurrencpp::async here, we'll just use the scheduler
				unsafe_promise<return_type> promise;
				deffered_schedueler scheduler;
				auto ret_future = promise.get_future();
				scheduler.schedule(std::move(promise), [future = std::move(future),
					function = std::forward<function_type>(function)]() mutable -> decltype(auto){
					future.wait(); //will cause the future to be ready
					return function(std::move(future));
				});

				return ret_future;
			}

			template<class return_type, class type, class function_type>
			::concurrencpp::future<return_type> set_then_ready(::concurrencpp::future<type> future, function_type&& function) {
				unsafe_promise<return_type> promise;
				promise.set_from_function([&]() -> decltype(auto) {
					return function(std::move(future));
				});
				return promise.get_future();
			}

			template<class type, class function_type, class return_type = typename std::result_of_t<function_type(::concurrencpp::future<type>)>>
			::concurrencpp::future<return_type> set_then(::concurrencpp::future<type> future, function_type&& function) {	
				auto state = future_result_status::NOT_READY;
				
				{
					std::unique_lock<decltype(m_lock)> lock(m_lock);

					assert(!static_cast<bool>(m_coro_handle));
					assert(!static_cast<bool>(m_then));
					state = m_status;

					if (state == future_result_status::NOT_READY) {
						return set_then_not_ready<return_type>(std::move(future), std::forward<function_type>(function));
					}
				}

				if (state == future_result_status::DEFFERED) {
					return set_then_deffered<return_type>(std::move(future), std::forward<function_type>(function));
				}

				assert(state == future_result_status::RESULT || state == future_result_status::EXCEPTION);
				return set_then_ready<return_type>(std::move(future), std::forward<function_type>(function));
			}
		};

		template<class type>
		class future_associated_state : public future_associated_state_base {

		private:
			compressed_future_result<type> m_result;

		public:
			future_associated_state() noexcept = default;
			~future_associated_state() noexcept { m_result.destroy(m_status); }

			template<class ... argument_types>
			void set_result(argument_types&& ... args) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_status == future_result_status::NOT_READY || m_status == future_result_status::DEFFERED);
				m_result.set_result(std::forward<argument_types>(args)...);
				m_status = future_result_status::RESULT;
				call_continuation();
			}

			void set_exception(std::exception_ptr exception_pointer) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_status == future_result_status::NOT_READY || m_status == future_result_status::DEFFERED);
				m_result.set_exception(std::move(exception_pointer));
				m_status = future_result_status::EXCEPTION;
				call_continuation();
			}

			type result_or_exception_unlocked() { return m_result.result_or_exception(m_status); }

			type result_or_exception() {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return m_result.result_or_exception(m_status);
			}

			type get() {
				wait();
				return m_result.result_or_exception(m_status);
			}
		};

		template<class type>
		class future_awaiter {
		
		private:
			std::shared_ptr<future_associated_state<type>> m_state;

		public:
			inline future_awaiter(decltype(m_state) state) noexcept : m_state(std::move(state)){}
			inline bool await_ready() const noexcept { return m_state->is_ready(); }
			
			inline void await_suspend(std::experimental::coroutine_handle<void> handle) noexcept { 
				m_state->set_coro_handle(handle); 
			}

			type await_resume() { 
				//we don't have to synchronize the result again. 
				//await_resume is called from "call_continuation -> coro_handle()"
				//and this function already uses the future lock.
				return m_state->result_or_exception_unlocked(); 
			}
		};

		template<class result_type, class ... argument_types>
		future<result_type> make_ready_future_impl(unsafe_promise<result_type>& promise, argument_types&& ... args) {
			static_assert(std::is_constructible_v<result_type, argument_types...>,
				"concurrencpp::make_ready_future<type>(args...) - cannot build type with given argument types.");
			promise.set_value(std::forward<argument_types>(args)...);
			return promise.get_future();
		}

		template<class ... argument_types>
		future<void> make_ready_future_impl(unsafe_promise<void>& promise, argument_types&& ... args) {
			static_assert(sizeof...(args) == 0,
				"concurrencpp::make_ready_future<void>() shouldn't get any parameters.");
			promise.set_value();
			return promise.get_future();
		}
		
		template<class type>
		class promise_base {

		protected:
			std::shared_ptr<future_associated_state<type>> m_state;
			bool m_future_retreived, m_fulfilled, m_moved;

			inline promise_base() noexcept : m_fulfilled(false), m_future_retreived(false), m_moved(false) {}
			
			inline promise_base(promise_base&& rhs) noexcept :
				m_state(std::move(rhs.m_state)),
				m_future_retreived(rhs.m_future_retreived),
				m_fulfilled(rhs.m_fulfilled),
				m_moved(rhs.m_moved) {
				rhs.m_moved = true;
			}

			promise_base& operator = (promise_base&& rhs) noexcept {
				break_promise_if_needed();

				m_state = std::move(rhs.m_state);
				m_moved = rhs.m_moved;
				m_fulfilled = rhs.m_fulfilled;
				m_future_retreived = rhs.m_future_retreived;

				rhs.m_moved = true;
				return *this;
			}

			void ensure_state() {
				if (m_moved) {
					throw std::future_error(std::future_errc::no_state);
				}

				if (m_fulfilled) {
					throw std::future_error(std::future_errc::promise_already_satisfied);
				}

				if (!m_state) {
					m_state = make_shared<future_associated_state<type>>();
				}
			}

			void break_promise_if_needed() {
				if (static_cast<bool>(m_state) && !m_fulfilled && !m_state->is_deffered_future()) {
					m_state->set_exception(std::make_exception_ptr(std::future_error(std::future_errc::broken_promise)));
				}
			}

		public:
			inline ~promise_base() noexcept { break_promise_if_needed(); }
			inline bool valid() const noexcept { return !m_moved; }

			::concurrencpp::future<type> get_future() {
				if (m_moved) {
					throw std::future_error(std::future_errc::no_state);
				}

				if (m_future_retreived) {
					throw std::future_error(std::future_errc::future_already_retrieved);
				}

				if (!static_cast<bool>(m_state)) {
					m_state = make_shared<future_associated_state<type>>();
				}

				m_future_retreived = true;
				return future<type>(m_state);
			}

			void set_exception(std::exception_ptr exception_pointer) {
				ensure_state();
				m_state->set_exception(exception_pointer);
				m_fulfilled = true;
			}
		};

		template<class scheduler_type, class function_type>
		auto async_impl(function_type&& function) {
			using result_type = std::result_of_t<function_type()>;

			scheduler_type scheduler;
			unsafe_promise<result_type> promise;
			auto future = promise.get_future();

			scheduler.schedule(std::move(promise), std::forward<function_type>(function));
			return future;
		}

		struct thread_scheduler {
			template<class type, class function_type>
			static void schedule(unsafe_promise<type> promise, function_type&& function) {	
				std::thread thread(promise.bind(std::forward<function_type>(function)));
				thread.detach();
			}
		};

		struct thread_pool_scheduler {
			template<class type, class function_type>
			static void schedule(unsafe_promise<type> promise, function_type&& function) {
				thread_pool::default_instance().enqueue_task(promise.bind(std::forward<function_type>(function)));
			}
		};

		struct blocked_scheduler {
			template<class type, class function_type>
			static void schedule(unsafe_promise<type> promise, function_type&& function) {
				promise.get_state()->set_scheduler(continuation_scheduler::thread_pool_scheduler);
				thread_pool::blocking_tasks_instance().enqueue_task(promise.bind(std::forward<function_type>(function)));
			}
		};

		struct deffered_schedueler {
			template<class type, class function_type>
			void schedule(unsafe_promise<type> promise, function_type&& function) {
				promise.set_deffered_task(std::forward<function_type>(function));
			}
		};

		template<class function_type>
		class bound_promise {
	
			using decayed_function_type = std::decay_t<function_type>;
			using result_type = std::result_of_t<function_type()>;	
	
		private:
			unsafe_promise<result_type> m_promise;
			decayed_function_type m_function;

		public:
			inline bound_promise(unsafe_promise<result_type> promise, function_type&& function) :
				m_promise(std::move(promise)),
				m_function(std::forward<function_type>(function)) {}

			inline void operator() () { m_promise.set_from_function(m_function); }	
		};

		template<class function_type>
		inline void set_future_state_from_function_impl(future_associated_state<void>& state, function_type&& function) {
			function();
			state.set_result();
		}

		template<class type, class function_type>
		inline void set_future_state_from_function_impl(future_associated_state<type>& state, function_type&& function) {
			state.set_result(function());
		}

		template<class type, class function_type>
		inline void set_future_state_from_function(future_associated_state<type>& state, function_type&& function) {
			try {
				set_future_state_from_function_impl(state, std::forward<function_type>(function));
			}
			catch (...) {
				state.set_exception(std::current_exception());
			}
		}

		template<class type>
		class unsafe_promise_base {
			//A promise that never checks, never throws. 
			//used as a light-result/exception vehicle for coroutines/library functions.	
		protected:
			std::shared_ptr<future_associated_state<type>> m_state;

			unsafe_promise_base(unsafe_promise_base&&) noexcept = default;

		public:
			inline unsafe_promise_base() { m_state = make_shared<future_associated_state<type>>(); }
			inline auto get_future() const noexcept { return future<type>(m_state); }
			inline void set_exception(std::exception_ptr exception) { m_state->set_exception(std::move(exception)); }
			inline auto get_state() const noexcept { return m_state; }	

			template<class function_type>
			inline void set_from_function(function_type&& function) {
				set_future_state_from_function(*m_state, std::forward<function_type>(function));
			}

			template<class function_type>
			void set_deffered_task(function_type&& function) {
				::std::weak_ptr<future_associated_state<type>> pointer = m_state;
				auto task = std::make_unique<deffered_future_task<type, function_type>>(std::move(pointer), std::forward<function_type>(function));
				m_state->set_deffered_task(std::move(task));
			}
		};

		template<class type>
		struct unsafe_promise : public unsafe_promise_base<type> {		
			unsafe_promise() = default;
			unsafe_promise(unsafe_promise&&) noexcept = default;

			template<class ... argument_types>
			void set_value(argument_types&& ... args) {
				this->m_state->set_result(std::forward<argument_types>(args)...);
			}

			template<class future_type>
			void set_from_future(future_type&& future) {
				try {
					this->m_state->set_result(future.get());
				}
				catch (...) { this->m_state->set_exception(std::current_exception()); }
			}	

			template<class function_type>
			inline bound_promise<function_type> bind(function_type&& function) {
				return bound_promise<function_type>(std::move(*this), std::forward<function_type>(function));
			}
		};

		template<>
		struct unsafe_promise<void> : public unsafe_promise_base<void> {
			unsafe_promise() = default;
			unsafe_promise(unsafe_promise&&) noexcept = default;

			inline void set_value() { m_state->set_result(); }
		
			template<class future_type>
			void set_from_future(future_type&& future) {
				try {
					future.get();
					this->m_state->set_result();
				}
				catch (...) { this->m_state->set_exception(std::current_exception()); }
			}

			template<class function_type>
			inline bound_promise<function_type> bind(function_type&& function) {
				return bound_promise<function_type>(std::move(*this), std::forward<function_type>(function));
			}
		};

		template<class type>
		class promise_type_base {
	
		protected:
			unsafe_promise<type> m_promise;

		public:		
			inline auto initial_suspend() const noexcept { return std::experimental::suspend_never(); }
			inline auto final_suspend() const noexcept { return std::experimental::suspend_never(); }
			inline ::concurrencpp::future<type> get_return_object() const noexcept { return m_promise.get_future(); }
			inline void set_exception(std::exception_ptr exception) { m_promise.set_exception(exception); }
		};

		inline size_t time_from_epoch() noexcept {
			const auto time_from_epoch_ = std::chrono::high_resolution_clock::now().time_since_epoch();
			return std::chrono::duration_cast<std::chrono::milliseconds>(time_from_epoch_).count();
		}

		class timer_impl {

		public:
			enum class status {
				IDLE,
				SCHEDULE,
				SCHEDULE_DELETE //For one timers
			};

		private:
			status update_timer_due_time(const size_t diff) noexcept;
			status update_timer_frequency(const size_t diff) noexcept;

		protected:
			size_t m_next_fire_time, m_last_update_time;
			std::atomic_size_t m_frequency;
			const size_t m_due_time;
			std::shared_ptr<timer_impl> m_next;
			std::weak_ptr<timer_impl> m_prev;
			bool m_has_due_time_reached;
			const bool m_is_oneshot;

		public:
			inline timer_impl(const size_t due_time, const size_t frequency, const bool is_oneshot) noexcept :
				m_due_time(due_time),
				m_next_fire_time(due_time),
				m_frequency(frequency),
				m_is_oneshot(is_oneshot),
				m_has_due_time_reached(false) {}

			virtual ~timer_impl() noexcept = default;

			status update() noexcept;
			void set_initial_update_time() noexcept;
			void cancel() noexcept;

			inline const size_t next_fire_time() const noexcept { return m_next_fire_time; }	
			inline void set_next(decltype(m_next) next) noexcept { m_next = std::move(next); }
			inline void set_prev(decltype(m_prev) prev) noexcept { m_prev = std::move(prev); }
			inline decltype(m_next) get_next() const noexcept { return m_next; }
			inline decltype(m_prev) get_prev() const noexcept { return m_prev; }

			inline void set_new_frequency_time(size_t new_frequency) noexcept { 
				m_frequency.store(new_frequency, std::memory_order_release);
			}

			virtual void execute() = 0;
		};

		template<class task_type>
		class concrete_timer final : public timer_impl, public pool_allocated {

			using decayed_function_type = typename std::decay_t<task_type>;

		private:
			decayed_function_type m_task;

		public:
			template<class function_type>
			concrete_timer(
				const size_t due_time,
				const size_t frequency,
				const bool is_oneshot,
				function_type&& function) :
				timer_impl(due_time, frequency, is_oneshot),
				m_task(std::forward<function_type>(function)) {}

			virtual void execute() override final { m_task(); }
		};

		//type erased
		template<class task_type>
		std::shared_ptr<timer_impl> make_timer_impl(
			const size_t due_time,
			const size_t frequency,
			const bool is_oneshot,
			task_type&& task) {
			pool_allocator<concrete_timer<task_type>> allocator;
			return std::allocate_shared<concrete_timer<task_type>>(
				allocator,
				due_time,
				frequency,
				is_oneshot,
				std::forward<task_type>(task));
		}

		template<class function_type, class ... argument_types>
		std::shared_ptr<timer_impl> make_timer_impl(
			const size_t due_time,
			const size_t frequency,
			const bool is_oneshot,
			function_type&& function,
			argument_types&& ... args) {
			return make_timer_impl(
				due_time,
				frequency,
				is_oneshot, 
				std::bind(std::forward<function_type>(function), std::forward<argument_types>(args)...));
		}

		void schedule_new_timer(std::shared_ptr<timer_impl> timer_ptr) noexcept;
	}

	template<class type>
	class future {

	private:
		std::shared_ptr<details::future_associated_state<type>> m_state;

		void throw_if_empty() const {
			if (!static_cast<bool>(m_state)) {
				throw std::future_error(std::future_errc::no_state);
			}
		}

	public:
		future(decltype(m_state) state) noexcept : m_state(std::move(state)) {}
		future() noexcept = default;
		future(future&& rhs) noexcept = default;
		future& operator = (future&& rhs) noexcept = default;

		future(const future& rhs) = delete;
		future& operator = (const future&) = delete;

		bool valid() const noexcept { return static_cast<bool>(m_state); }

		void wait() {
			throw_if_empty();
			m_state->wait();
		}

		template<class duration_unit, class ratio>
		::std::future_status wait_for(std::chrono::duration<duration_unit, ratio> duration) const {
			throw_if_empty();
			return m_state->wait_for(duration);
		}

		template< class clock, class duration >
		std::future_status wait_until(const std::chrono::time_point<clock, duration>& timeout_time) const {
			const auto diff = timeout_time - std::chrono::system_clock::now();
			return wait_for(diff);
		}

		type get() {
			static_assert(std::is_reference_v<type> ||
				std::is_same_v<void,type> ||
				std::is_move_constructible_v<type> ||
				std::is_move_assignable_v<type>,
				"concurrencpp::future<type>::get - type has to be void, reference type, move constructible or move assignable");
			
			throw_if_empty();
			future _this(std::move(*this));
			return _this.m_state->get();
		}

		template<class continuation_type, class new_type = typename std::result_of_t<continuation_type(future<type>)>>	
		future<new_type> then(continuation_type&& continuation) {
			throw_if_empty();
			auto this_state = m_state; //important save a copy of the shared state.
			return this_state->set_then(std::move(*this), std::forward<continuation_type>(continuation));
		}

		auto operator co_await() {
			throw_if_empty();
			return details::future_awaiter<type>(std::move(m_state));
		}
	};

	template<class result_type, class ... argument_types>
	future<result_type> make_ready_future(argument_types&& ... args) {
		details::unsafe_promise<result_type> promise;
		return details::make_ready_future_impl(promise, std::forward<argument_types>(args)...);
	}
	
	template<class result_type, class exception_type, class ... argument_types>
	future<result_type> make_exceptional_future(argument_types&& ... args) {
		static_assert(std::is_constructible_v<exception_type, argument_types...>,
			"concurrencpp::make_exceptional_future() - cannot build exception with given arguments.");
		return make_exceptional_future<result_type>(std::make_exception_ptr(exception_type(std::forward<argument_types>(args)...)));
	}

	template<class result_type>
	future<result_type> make_exceptional_future(std::exception_ptr exception_pointer) {
		details::unsafe_promise<result_type> promise;
		promise.set_exception(exception_pointer);
		return promise.get_future();
	}

	template<class type>
	struct promise : public details::promise_base<type> {
		promise() noexcept = default;
		promise(promise&& rhs) noexcept : details::promise_base<type>(std::move(rhs)) {}

		promise& operator = (promise&& rhs) noexcept {
			this->promise_base::operator = (std::move(rhs));
			return *this;
		}

		template<class ... argument_types>
		void set_value(argument_types&& ... args) {
			static_assert(std::is_constructible_v<type,argument_types...>,
				"concurrencpp::promise<type>::set_value - cannot build type with given argument types.");
			this->ensure_state();
			this->m_state->set_result(std::forward<argument_types>(args)...);
			this->m_fulfilled = true;
		}

		void swap(promise& rhs) noexcept {
			auto temp = std::move(rhs);
			rhs = std::move(*this);
			*this = std::move(temp);
		}
	};

	template<>
	struct promise<void> : public details::promise_base<void> {
		promise() noexcept = default;
		promise(promise&& rhs) noexcept : promise_base(std::move(rhs)) {}

		promise& operator = (promise&& rhs) noexcept {
			this->promise_base::operator = (std::move(rhs));
			return *this;
		}

		void set_value() {
			this->ensure_state();
			this->m_state->set_result();
			this->m_fulfilled = true;
		}
	};

	template<class function_type, class ... argument_types>
	void spawn(function_type&& function, argument_types&& ... args) {
		details::thread_pool::default_instance().enqueue_task(
			std::forward<function_type>(function),
			std::forward<argument_types>(args)...);
	}

	template<class function_type>
	void spawn(function_type&& function) {
		details::thread_pool::default_instance().enqueue_task(std::forward<function_type>(function));
	}

	template<class function_type, class ... argument_types>
	void spawn_blocked(function_type&& function, argument_types&& ... args) {
		details::thread_pool::blocking_tasks_instance().enqueue_task(
			std::forward<function_type>(function),
			std::forward<argument_types>(args)...);
	}

	template<class function_type>
	void spawn_blocked(function_type&& function) {
		details::thread_pool::blocking_tasks_instance().enqueue_task(std::forward<function_type>(function));
	}

	enum class launch {
		async,
		deferred,
		task,
		blocked_task
	};

	template <class function_type>
	auto async(launch launch_policy, function_type&& function) {
		switch (launch_policy) {
		case ::concurrencpp::launch::task: {
			return details::async_impl<details::thread_pool_scheduler>(std::forward<function_type>(function));
		}
		case ::concurrencpp::launch::blocked_task: {
			return details::async_impl<details::blocked_scheduler>(std::forward<function_type>(function));
		}
		case ::concurrencpp::launch::async: {
			return details::async_impl<details::thread_scheduler>(std::forward<function_type>(function));
		}
		case ::concurrencpp::launch::deferred: {
			return details::async_impl<details::deffered_schedueler>(std::forward<function_type>(function));
		}
		}

		assert(false);
		return future<typename std::result_of_t<function_type()>>();
	}

	template <class function_type, class... argument_types>
	auto async(launch launch_policy, function_type&& function, argument_types&&... args) {
		return async(launch_policy, std::bind(
			std::forward<function_type>(function),
			std::forward<argument_types>(args)...));
	}

	template <class function_type, class... argument_types>
	auto async(function_type&& function, argument_types&&... arguments) {
		return async(launch::task,
			std::forward<function_type>(function),
			std::forward<argument_types>(arguments)...);
	}

	template <class function_type>
	auto async(function_type&& function) { return async(launch::task, std::forward<function_type>(function)); }

} //namespace concurrencpp

namespace concurrencpp {
	namespace details {

		template<class ... future_types>
		class when_all_state : public std::enable_shared_from_this<when_all_state<future_types...>> {

		private:
			std::tuple<future_types...> m_tuple;
			std::atomic_size_t m_counter;
			unsafe_promise<decltype(m_tuple)> m_promise;

			template<class type, size_t index>
			void on_future_ready(::concurrencpp::future<type> done_future) {
				std::get<index>(m_tuple) = std::move(done_future);
				if (m_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
					m_promise.set_value(std::move(m_tuple));
				}
			}

			template<size_t index> void set_future_then() const noexcept {}

			template<size_t index, class type, class ... future_types>
			void set_future_then(::concurrencpp::future<type>& future, future_types&& ... futures) {
				future.then([_this = this->shared_from_this()](::concurrencpp::future<type> done_future){
					_this->on_future_ready<type, index>(std::move(done_future));
				});

				set_future_then<index + 1>(std::forward<future_types>(futures)...);
			}

		public:
			template<class ... future_types>
			void set_futures(future_types&& ... futures) {
				set_future_then<0>(std::forward<future_types>(futures)...);
				m_counter = sizeof ... (future_types);
			}

			auto get_future() const noexcept { return m_promise.get_future(); }
		};

	}

	inline future<::std::tuple<>> when_all() {
		return ::concurrencpp::make_ready_future<::std::tuple<>>();
	}

	template<class ... future_types>
	future<std::tuple<typename std::decay<future_types>::type...>>
		when_all(future_types&& ... futures) {
		auto when_all_state = details::make_shared<details::when_all_state<typename std::decay<future_types>::type...>>();
		when_all_state->set_futures(std::forward<future_types>(futures)...);
		return when_all_state->get_future();
	}
}

namespace concurrencpp {	
	template <class sequence_type>
	struct when_any_result {
		const std::size_t index;
		sequence_type futures;

		when_any_result() noexcept : index(static_cast<size_t>(-1)) {}

		template<class ... future_types>
		when_any_result(size_t index, future_types&& ... futures) noexcept:
			index(index),
			futures(std::forward<future_types>(futures)...) {}
	};

	namespace details {
		template<class sequence_type>
		class when_any_state : public std::enable_shared_from_this<when_any_state<sequence_type>>{
			
		private:
			unsafe_promise<::concurrencpp::when_any_result<sequence_type>> m_promise;
			sequence_type m_futures;
			::std::atomic_bool m_fulfilled;

			template<class type>
			type on_task_finished(::concurrencpp::future<type> future, size_t index) {
				const auto fulfilled = std::atomic_exchange_explicit(
					&m_fulfilled,
					true,
					std::memory_order_acquire);

				if (fulfilled == false) { //this is the first finished task
					m_promise.set_value(index, std::move(m_futures));
				}

				return future.get();
			}

			template<size_t index, class type>
			void set_future(::concurrencpp::future<type>& future) noexcept {
				assert(future.valid());
				std::get<index>(m_futures) = 
					future.then([_this = this->shared_from_this()](::concurrencpp::future<type> future) mutable -> type {
					return _this->on_task_finished(std::move(future), index);
				});
			}

			template<size_t index> void set_futures() const noexcept {}

			template<size_t index, class future_type, class ... future_types>
			void set_futures(future_type&& future, future_types&& ... futures) noexcept {
				set_future<index>(std::forward<future_type>(future));
				set_futures<index + 1>(std::forward<future_types>(futures)...);
			}

		public:
			when_any_state() noexcept : m_fulfilled(false) {}
			
			auto get_future() const noexcept { return m_promise.get_future(); }

			template<class ... future_types>
			void set_futures(future_types&& ... futures) noexcept {
				set_futures<0>(std::forward<future_types>(futures)...);
			}
		};
	}

	inline future<when_any_result<::std::tuple<>>> when_any() {
		return make_ready_future<when_any_result<::std::tuple<>>>();
	}

	template<class ... future_types>
	auto when_any(future_types&& ... futures) {
		using tuple_t = std::tuple<std::decay_t<future_types>...>;
		auto state = details::make_shared<details::when_any_state<tuple_t>>();
		state->set_futures(std::forward<future_types>(futures)...);
		return state->get_future();
	}
}

namespace concurrencpp {

	struct empty_timer : public empty_object_exception {
		empty_timer(const char* error_messgae) : empty_object_exception(error_messgae) {}
	};

	class timer {

	private:
		std::shared_ptr<details::timer_impl> m_impl;

		void throw_if_empty() const {
			if (static_cast<bool>(m_impl)) {
				return;
			}

			throw empty_timer("concurrencpp::timer - timer is empty.");
		}

		void cancel_if_not_empty() noexcept {
			if (static_cast<bool>(m_impl)) {
				m_impl->cancel();
			}
		}

		template<class function_type, class ... arguments_type>
		static void init_timer(std::shared_ptr<details::timer_impl>& timer_ptr,
			size_t due_time, size_t frequency, bool is_oneshot,
			function_type&& function, arguments_type&& ... args) {
			timer_ptr = details::make_timer_impl(due_time, frequency, is_oneshot,
				std::forward<function_type>(function), std::forward<arguments_type>(args)...);

			schedule_new_timer(timer_ptr);
		}

	public:
		template<class function_type, class ... arguments_type>
		timer(size_t due_time, size_t frequency, function_type&& function, arguments_type&& ... args) {
			init_timer(m_impl, due_time, frequency, false,
				std::forward<function_type>(function), std::forward<arguments_type>(args)...);
		}

		inline ~timer() noexcept { cancel_if_not_empty(); }

		timer(const timer&) = delete;
		timer(timer&& rhs) noexcept = default;

		timer& operator = (const timer&) = delete;

		timer& operator = (timer&& rhs) noexcept{
			if (this == &rhs) {
				return *this;
			}

			cancel_if_not_empty();
			m_impl = std::move(rhs.m_impl);
			return *this;
		}

		void cancel() {
			throw_if_empty();
			m_impl->cancel();
			m_impl.reset();
		}

		void set_frequency(size_t new_frequency) {
			throw_if_empty();
			m_impl->set_new_frequency_time(new_frequency);
		}

		bool valid() const noexcept { return static_cast<bool>(m_impl); }

		template<class function_type, class ... arguments_type>
		static void once(size_t due_time, function_type&& function, arguments_type&& ... args) {
			std::shared_ptr<details::timer_impl> ignored;
			init_timer(
				ignored,
				due_time,
				std::numeric_limits<size_t>::max(),
				true,
				std::forward<function_type>(function),
				std::forward<arguments_type>(args)...);
		}

		template<class ignored_type = void>
		static future<void> delay(size_t due_time) {
			details::unsafe_promise<void> promise;
			auto future = promise.get_future();
			once(due_time, [promise = std::move(promise)]() mutable {
				promise.set_value();
			});

			return future;
		}
	};
}

namespace std {
	namespace experimental {

		template<class type, class... arguments>
		struct coroutine_traits<::concurrencpp::future<type>, arguments...> {	
			struct promise_type : 
				public concurrencpp::details::promise_type_base<type> ,
				public concurrencpp::details::pool_allocated {

				template<class return_type>
				void return_value(return_type&& value) { this->m_promise.set_value(std::forward<return_type>(value)); }
			};
		};

		template<class... arguments>
		struct coroutine_traits<::concurrencpp::future<void>, arguments...> {
			struct promise_type :
				public concurrencpp::details::promise_type_base<void>,
				public concurrencpp::details::pool_allocated {

				void return_void() { this->m_promise.set_value(); }
			};
		};

		template<class ... arguments>
		struct coroutine_traits<void, arguments...> {
			struct promise_type : public concurrencpp::details::pool_allocated {
				promise_type() noexcept {}
				void get_return_object() noexcept {}
				auto initial_suspend() const noexcept { return suspend_never{}; }
				auto final_suspend() const noexcept { return suspend_never{}; }
				void return_void() noexcept {}

				template<class exception_type>
				void set_exception(exception_type&& exception) noexcept {}
			};
		};
	}	//experimental
}	//std

#endif