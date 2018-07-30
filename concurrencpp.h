/*
	Copyright (c) 2017 David Haim

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
#include <queue>
#include <array>

#include <memory>
#include <chrono>
#include <cassert>
#include <type_traits>

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <experimental\coroutine>

namespace concurrencpp {
	namespace details {

		class spinlock {
			constexpr static size_t locked = 1;
			constexpr static size_t unlocked = 0;
			constexpr static size_t spin_count = 128;

		private:
			std::atomic_size_t m_lock;

			static void pause_cpu() noexcept {
#if (COMPILER == MVCC)
				_mm_pause();
#elif (COMPILER == GCC || COMPILER == LLVM)
				asm volatile("pause\n": : : "memory");		
#endif
			}

			template<class on_fail>
			bool try_lock_once(on_fail&& on_fail_callable) {
				size_t counter = 0ul;

				//make the processor yield
				while (true) {
					const auto state = std::atomic_exchange_explicit(
						&m_lock,
						locked,
						std::memory_order_acquire);

					if (state == unlocked) {
						return true;
					}

					if (counter == spin_count) {
						break;
					}

					++counter;
					on_fail_callable();
				}

				return false;
			}

		public:

			spinlock() noexcept : m_lock(unlocked) {}

			void lock() {
				if (try_lock_once([] { pause_cpu(); })) {
					return;
				}

				if (try_lock_once([] { std::this_thread::yield(); })) {
					return;
				}

				while (try_lock_once([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }) == false);
			}

			void unlock() noexcept { m_lock.store(unlocked, std::memory_order_release); }

			bool try_lock() noexcept {
				return std::atomic_exchange_explicit(
					&m_lock,
					true,
					std::memory_order_acquire) == unlocked;
			}

		};

		class recursive_spinlock {

		private:
			std::atomic<std::thread::id> m_owner;
			intmax_t m_count;

		public:

			recursive_spinlock() noexcept : m_count(0) {}

			void lock() noexcept {
				const auto this_thread = std::this_thread::get_id();
				std::thread::id no_id;
				if (m_owner != this_thread) {
					while (!m_owner.compare_exchange_weak(
						no_id,
						this_thread,
						std::memory_order_acquire,
						std::memory_order_relaxed)) {
						no_id = {};
						std::this_thread::yield();
					}
				}

				m_count++;
			}

			void unlock() noexcept {
				assert(m_owner == std::this_thread::get_id());
				assert(m_count > 0);

				--m_count;
				if (m_count == 0) {
					m_owner.store(std::thread::id(), std::memory_order_release);
				}
			}

		};

		struct memory_block {
			memory_block* next;
		};

		class block_list {

		private:
			memory_block* m_head;
			size_t m_block_count;

		public:

			block_list() noexcept : m_head(nullptr), m_block_count(0ul) {}
			~block_list() noexcept { clear(); }

			void clear() noexcept {
				auto cursor = m_head;
				while (cursor != nullptr) {
					auto temp = cursor;
					cursor = cursor->next;
					std::free(temp);
				}

				m_head = nullptr;
				m_block_count = 0;
			}

			void* allocate() noexcept {
				if (m_head == nullptr) {
					return nullptr;
				}

				auto block = m_head;
				m_head = m_head->next;
				--m_block_count;

				assert(m_block_count == 0 ?
					(m_head == nullptr) :
					(m_head != nullptr));

				return block;
			}

			void deallocate(void* chunk) noexcept{
				auto block = static_cast<memory_block*>(chunk);
				block->next = m_head;
				m_head = block;
				++m_block_count;
			}

			size_t get_block_count() const noexcept { return m_block_count; }
		};

		class memory_pool {
			static constexpr size_t MAX_BLOCK_COUNT = 1024 * 64;
			static constexpr size_t DEFAULT_POOL_SIZE = 4;
			static constexpr size_t MEMORY_BLOCK_COUNT = 8;

			using synchronized_list_type = std::pair<block_list, spinlock>;
			using pool_type = std::array<synchronized_list_type, MEMORY_BLOCK_COUNT>;
			//pool = [32, 64 , 96, 128, 192, 256, 384, 512]

		private:
			std::vector<pool_type> m_pools;

			static size_t calculate_pool_size() noexcept {
				const auto number_of_cpus = std::thread::hardware_concurrency();
				return (number_of_cpus == 0) ? DEFAULT_POOL_SIZE : number_of_cpus;
			}
			
			static size_t align_size(const size_t unaligned_size) noexcept {
				if (unaligned_size <= 32) {
					return 32;
				}
				else if (unaligned_size <= 64) {
					return 64;
				}
				else if (unaligned_size <= 96) {
					return 96;
				}
				else if (unaligned_size <= 128) {
					return 128;
				}
				else if (unaligned_size <= 192) {
					return 192;
				}
				else if (unaligned_size <= 256) {
					return 256;
				}
				else if (unaligned_size <= 384) {
					return 384;
				}
				else if (unaligned_size <= 512) {
					return 512;
				}

				assert(false);
				return static_cast<size_t>(-1);
			}

			static size_t find_bucket_index(const size_t unaligned_size) noexcept {
				if (unaligned_size <= 32) {
					return 0;
				}
				else if (unaligned_size <= 64) {
					return 1;
				}
				else if (unaligned_size <= 96) {
					return 2;
				}
				else if (unaligned_size <= 128) {
					return 3;
				}
				else if (unaligned_size <= 192) {
					return 4;
				}
				else if (unaligned_size <= 256) {
					return 5;
				}
				else if (unaligned_size <= 384) {
					return 6;
				}
				else if (unaligned_size <= 512) {
					return 7;
				}

				assert(false);
				return static_cast<size_t>(-1);
			}

			static memory_pool& instance() {
				static memory_pool s_memory_pool;
				return s_memory_pool;
			}

			static size_t get_pool_index(size_t number_of_pools) {
				static thread_local const size_t index = [] {
					auto this_thread_id = std::this_thread::get_id();
					std::hash<decltype(this_thread_id)> hasher;
					return hasher(this_thread_id) % instance().m_pools.size();
				}();

				return index;
			}

		public:

			memory_pool() : m_pools(calculate_pool_size()) {}

			~memory_pool() {
				for (auto& pool : m_pools) {
					for (auto& bucket : pool) {
						std::lock_guard<decltype(bucket.second)> lock(bucket.second);
						bucket.first.clear();
					}
				}
			}

			static void* allocate(size_t chunk_size) {
				assert(chunk_size != 0);

				void* memory = nullptr;
				if (chunk_size > 512) {
					memory = std::malloc(chunk_size);
					if (memory != nullptr) {
						return memory;
					}

					throw std::bad_alloc();
				}

				auto& _this = instance();
				auto& pools = _this.m_pools;
				auto& pool = pools[get_pool_index(pools.size())];

				const auto bucket_index = find_bucket_index(chunk_size);
				assert(bucket_index < pool.size());
				auto& bucket = pool[bucket_index];

				{
					std::lock_guard<decltype(bucket.second)> lock(bucket.second);
					memory = bucket.first.allocate();
				}

				if (memory != nullptr) {
					return memory;
				}

				//try stealing
				const auto total = pools.size();
				for (size_t i = 0; i < total; i++) {
					auto& bucket = pools[i % total][bucket_index];
					std::lock_guard<decltype(bucket.second)> lock(bucket.second);
					memory = bucket.first.allocate();

					if (memory != nullptr) {
						return memory;
					}
				}

				chunk_size = align_size(chunk_size);
				memory = std::malloc(chunk_size);
				if (memory != nullptr) {
					return memory;
				}

				throw std::bad_alloc();
			}

			static void deallocate(void* chunk, size_t size) noexcept {
				assert(size != 0);

				if (size > 512) {
					std::free(chunk);
					return;
				}

				auto& _this = instance();
				auto& pools = _this.m_pools;

				auto this_thread_id = std::this_thread::get_id();
				std::hash<decltype(this_thread_id)> hasher;
				auto pool_index = hasher(this_thread_id) % pools.size();
				auto& pool = pools[pool_index];

				const auto bucket_index = find_bucket_index(size);
				auto& bucket = pool[bucket_index];

				std::lock_guard<decltype(bucket.second)> lock(bucket.second);
				const auto count = bucket.first.get_block_count();
				if (count < MAX_BLOCK_COUNT) {
					bucket.first.deallocate(chunk);
				}
				else {
					std::free(chunk);
				}
			}
		};

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

			type* allocate(const size_t n) const {
				if (n == 0) {
					return nullptr;
				}

				return static_cast<type*>(memory_pool::allocate(n * sizeof(type)));
			}

			void deallocate(type* const block, const size_t size) const noexcept {
				memory_pool::deallocate(block, size * sizeof(type));
			}
		};

		struct pool_allocated {
			static void* operator new(const size_t size) { return memory_pool::allocate(size); }
			static void operator delete(void* block, size_t size) { memory_pool::deallocate(block, size); }
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

		struct callback_base {
			virtual ~callback_base() = default;
			virtual void execute() = 0;

			std::unique_ptr<callback_base> next;
		};

		template<class function_type>
		struct callback : public callback_base, public pool_allocated {

		private:
			function_type m_function;

		public:
			callback(function_type&& function) : m_function(std::forward<function_type>(function)) {}

			virtual void execute() override final { m_function(); }
		};

		template<class function_type>
		std::unique_ptr<callback_base> make_callback(function_type&& function) {
			return std::unique_ptr<callback_base>(
				new callback<function_type>(std::forward<function_type>(function)));
		}

		class work_queue {

		private:
			std::unique_ptr<callback_base> m_head;
			callback_base* m_tail;

		public:

			work_queue() noexcept : m_tail(nullptr){}

			void push(decltype(m_head) task) noexcept {
				if (m_head == nullptr) {
					m_tail = task.get();
					m_head = std::move(task);
				}
				else {
					m_tail->next = std::move(task);
					m_tail = m_tail->next.get();
				}
			}

			auto try_pop() noexcept {
				if (m_head.get() == nullptr) {
					return decltype(m_head){};
				}

				auto& next = m_head->next;
				auto task = std::move(m_head);
				m_head = std::move(next);

				if (m_head == nullptr) {
					m_tail = nullptr;
				}

				return task;
			}

			bool empty() const noexcept {
				return !static_cast<bool>(m_head);
			}

		};

		class worker_thread {

		private:
			spinlock m_lock;
			work_queue m_tasks;
			std::vector<worker_thread>& m_thread_group;
			std::atomic_bool& m_stop;
			std::condition_variable_any m_condition;
			std::thread m_worker;

			void wait_for_pool_construction(std::mutex& construction_lock) {
				std::lock_guard<decltype(construction_lock)> lock(construction_lock);
			}

			void work(std::mutex& construction_lock) {
				wait_for_pool_construction(construction_lock);

				while (!m_stop.load(std::memory_order_acquire)) {
					std::unique_ptr<callback_base> task;

					{
						std::lock_guard<decltype(m_lock)> lock(m_lock);
						task = m_tasks.try_pop();
					}

					if (task) {
						task->execute();
						continue;
					}

					for (auto& worker : m_thread_group) {
						task = worker.try_steal();
						if (task) {
							break;
						}
					}

					if (task) {
						task->execute();
						continue;
					}

					std::unique_lock<decltype(m_lock)> lock(m_lock);
					m_condition.wait(lock, [&] {
						return !m_tasks.empty() || m_stop.load(std::memory_order_acquire);
					});

				}
			}

			std::unique_ptr<callback_base> try_steal() noexcept {
				std::unique_lock<decltype(m_lock)> lock(m_lock, std::try_to_lock);

				if (lock.owns_lock() && !m_tasks.empty()) {
					return m_tasks.try_pop();
				}

				return{};
			}

		public:

			worker_thread(
				std::vector<worker_thread>& thread_group,
				std::atomic_bool& stop,
				std::mutex& construction_lock) :
				m_thread_group(thread_group),
				m_stop(stop),
				m_worker([this, &construction_lock] {work(construction_lock); }) {}

			worker_thread(worker_thread&& rhs) noexcept :
				m_tasks(std::move(rhs.m_tasks)),
				m_stop(rhs.m_stop),
				m_thread_group(rhs.m_thread_group) {}

			template<class function_type>
			void enqueue_task(bool self, function_type&& function) {
				enqueue_task(make_callback(std::forward<function_type>(function)));
			}

			void enqueue_task(bool self, std::unique_ptr<callback_base> task) {
				{
					std::unique_lock<decltype(m_lock)> lock(m_lock);
					m_tasks.push(std::move(task));
				}

				if (!self) {
					m_condition.notify_one();
				}
			}

			void notify() {
				m_condition.notify_one();
			}

			void join() {
				m_worker.join();
			}

			std::thread::id get_id() const noexcept {
				return m_worker.get_id();
			}
		};

		class thread_pool {

		private:
			std::vector<worker_thread> m_workers;
			std::atomic_bool m_stop;
			std::mutex m_pool_lock;

			size_t choose_next_worker() noexcept {
				static thread_local size_t counter = 0;
				return ++counter % m_workers.size();
			}

			worker_thread* get_self_worker_impl() noexcept {
				const auto this_thread_id = std::this_thread::get_id();
				for (auto& worker : m_workers) {
					if (worker.get_id() == this_thread_id) {
						return &worker;
					}
				}

				return nullptr;
			}

			worker_thread* get_self_worker() noexcept {
				static thread_local auto cached_worker_thread =
					get_self_worker_impl();
				return cached_worker_thread;
			}

			static size_t number_of_threads_cpu() {
				const auto concurrency_level = std::thread::hardware_concurrency();
				return concurrency_level == 0 ? 8 : static_cast<size_t>(concurrency_level * 1.25f);
			}

			static size_t number_of_threads_io() {
				const auto concurrency_level = std::thread::hardware_concurrency();
				return concurrency_level == 0 ? 8 : (concurrency_level * 2);
			}

		public:

			thread_pool(const size_t number_of_workers) :
				m_stop(false) {
				std::lock_guard<std::mutex> construction_lock(m_pool_lock);
				m_workers.reserve(number_of_workers);
				for (auto i = 0ul; i < number_of_workers; i++) {
					m_workers.emplace_back(m_workers, m_stop, m_pool_lock);
				}
			}

			~thread_pool() {
				m_stop.store(true, std::memory_order_release);

				for (auto& worker : m_workers) {
					worker.notify();
				}

				std::this_thread::yield();

				for (auto& worker : m_workers) {
					worker.join();
				}
			}

			template<class function_type, class ... arguments>
			void enqueue_task(function_type&& function, arguments&& ... args) {
				enqueue_task(std::bind(std::forward<function_type>(function), std::forward<arguments>(args)...));
			}

			template<class function_type>
			void enqueue_task(function_type&& function) {
				enqueue_task(make_callback(std::forward<function_type>(function)));
			}

			void enqueue_task(std::unique_ptr<callback_base> function) {
				auto self_worker = get_self_worker();

				if (self_worker != nullptr) {
					self_worker->enqueue_task(true, std::move(function));
					return;
				}

				const auto index = choose_next_worker();
				m_workers[index].enqueue_task(false, std::move(function));
			}

			static thread_pool& default_instance() {
				static thread_pool s_default_thread_pool(number_of_threads_cpu());
				return s_default_thread_pool;
			}

			static thread_pool& blocking_tasks_instance() {
				static thread_pool s_blocking_tasks_thread_pool(number_of_threads_io());
				return s_blocking_tasks_thread_pool;
			}
		};

		struct promise_setter {

			template<class function_type, class type>
			static void execute_set_promise(::concurrencpp::promise<type>& promise, function_type&& function) noexcept {
				try {
					promise.set_value(function());
				}
				catch (...) {
					promise.set_exception(std::current_exception());
				}
			}

			template<class function_type>
			static void execute_set_promise(::concurrencpp::promise<void>& promise, function_type&& function) noexcept {
				try {
					function();
					promise.set_value();
				}
				catch (...) {
					promise.set_exception(std::current_exception());
				}
			}
		};

		enum class future_result_state {
			NOT_READY,
			RESULT,
			EXCEPTION,
			DEFFERED
		};

		template<class type>
		union compressed_future_result {
			type result;
			std::exception_ptr exception;

			compressed_future_result() noexcept {};
			~compressed_future_result() noexcept {};
		};

		template<class type>
		union compressed_future_result<type&> {
			type* result;
			std::exception_ptr exception;

			compressed_future_result() noexcept {};
			~compressed_future_result() noexcept {};
		};

		template<class type, class function_type>
		class future_then final : public callback_base, public pool_allocated {

			using new_type = typename std::result_of_t<function_type(::concurrencpp::future<type>)>;

		private:
			::concurrencpp::future<type> m_future;
			::concurrencpp::promise<new_type> m_promise;
			function_type m_function;

		public:

			future_then(::concurrencpp::future<type> future, function_type&& function) :
				m_future(std::move(future)),
				m_function(std::forward<function_type>(function)) {}

			void execute() noexcept final {
				promise_setter::execute_set_promise(m_promise, [this] {
					return m_function(std::move(m_future));
				});
			}

			::concurrencpp::future<new_type> get_future() { return m_promise.get_future(); }
		};

		class future_associated_state_base {
	
		protected:
			//members are ordered in the order of their importance.
			mutable recursive_spinlock m_lock;
			future_result_state m_state;
			::std::experimental::coroutine_handle<void> m_coro_handle;
			std::unique_ptr<callback_base> m_then, m_deffered;
			mutable std::unique_ptr<std::condition_variable_any> m_condition; //lazy creation

			void build_condition_object() const {
				pool_allocator<std::condition_variable_any> allocator;
				auto cv = allocator.allocate(1);
				try {
					new (cv) std::condition_variable_any();
				}
				catch (...) {
					allocator.deallocate(cv, 1);
					throw;
				}

				m_condition.reset(cv);
			}

		public:

			future_associated_state_base() noexcept : m_state(future_result_state::NOT_READY) {}

			std::condition_variable_any& get_condition() {
				if (!static_cast<bool>(m_condition)) {
					build_condition_object();
				}

				assert(static_cast<bool>(m_condition));
				return *m_condition;
			}

			const std::condition_variable_any& get_condition() const {
				if (!static_cast<bool>(m_condition)) {
					build_condition_object();
				}

				assert(static_cast<bool>(m_condition));
				return *m_condition;
			}

			template<class duration_unit, class ratio>
			::std::future_status wait_for(std::chrono::duration<duration_unit, ratio> duration) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);

				if (m_state == future_result_state::DEFFERED) {
					return ::std::future_status::deferred;
				}

				if (m_state != future_result_state::NOT_READY) {
					return ::std::future_status::ready;
				}

				auto& condition = get_condition();
				const auto has_result = condition.wait_for(lock, duration, [this] {
					return !(m_state == future_result_state::NOT_READY);
				});

				return has_result ? std::future_status::ready : std::future_status::timeout;
			}

			void wait() {
				/*
					According to the standard, only non-timed wait on the future
					will cause the deffered-function to be launched. this is why
					this segment is not in wait_for implementation.
				*/

				{
					std::unique_lock<decltype(m_lock)> lock(m_lock);
					if (m_state == future_result_state::DEFFERED) {
						assert(static_cast<bool>(m_deffered));
						m_deffered->execute();

						assert(m_state != future_result_state::DEFFERED);
						assert(m_state != future_result_state::NOT_READY);
						return;
					}
				}

				while (wait_for(std::chrono::hours(365 * 24)) == std::future_status::timeout);
			}

			bool has_deffered_task() const noexcept { return m_state == future_result_state::DEFFERED; }

			bool is_ready() const noexcept {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return m_state == future_result_state::EXCEPTION || m_state == future_result_state::RESULT;
			}

			void set_deffered_task(std::unique_ptr<callback_base> task) noexcept {
				// this function should only be called once, by using async + launch::deffered
				std::unique_lock<decltype(m_lock)> lock(m_lock);

				assert(m_state == future_result_state::NOT_READY);
				assert(!static_cast<bool>(m_deffered));
				assert(!static_cast<bool>(m_coro_handle));
				assert(!static_cast<bool>(m_then));

				m_deffered = std::move(task);
				m_state = future_result_state::DEFFERED;
			}

			template<class type, class function_type, class return_type = typename std::result_of_t<function_type(::concurrencpp::future<type>)>>
			::concurrencpp::future<return_type> set_then(::concurrencpp::future<type> future, function_type&& function) noexcept {	
				std::unique_lock<decltype(m_lock)> lock(m_lock);

				assert(!static_cast<bool>(m_coro_handle));
				assert(!static_cast<bool>(m_then));

				if (m_state == future_result_state::RESULT || m_state == future_result_state::EXCEPTION) {
					::concurrencpp::promise<return_type> promise;
					promise_setter::execute_set_promise(promise, [&function, &future] {return function(std::move(future)); });
					return promise.get_future();
				}
				
				if (m_state == future_result_state::DEFFERED) {
					return ::concurrencpp::async(::concurrencpp::launch::deferred,
						[future = std::move(future), function = std::forward<function_type>(function)]() mutable {
							future.wait(); //will cause the future to be ready
							return function(std::move(future));
					});
				}

				auto then = std::make_unique<future_then<type, function_type>>(std::move(future), std::forward<function_type>(function));
				auto ret_future = then->get_future();
				m_then.reset(then.release());	
				return ret_future;
			}

			void set_coro_handle(decltype(m_coro_handle) coro_handle) noexcept {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				
				assert(!static_cast<bool>(m_deffered));
				assert(!static_cast<bool>(m_coro_handle));
				assert(!static_cast<bool>(m_then));
				
				//It could be that in between the time of await_ready() and await_suspend() the result was set. recheck
				if (m_state == future_result_state::RESULT || m_state == future_result_state::EXCEPTION) {
					return coro_handle();
				}
				
				m_coro_handle = coro_handle;
			}

			void set_exception(std::exception_ptr exception_pointer, void* exception_storage) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				new (exception_storage) std::exception_ptr(std::move(exception_pointer));
				m_state = future_result_state::EXCEPTION;
				call_continuation();
			}

			void call_continuation() {
				if (static_cast<bool>(m_condition)) {
					m_condition->notify_all(); //do not reset the CV, as other objects wait on it on another thread.
				}

				if (static_cast<bool>(m_coro_handle)) {
					return m_coro_handle();
				}

				if (static_cast<bool>(m_then)) {
					return m_then->execute();
				}
			}
		};

		template<class type>
		class future_associated_state : public future_associated_state_base {

		private:
			compressed_future_result<type> m_result;

		public:

			future_associated_state() noexcept = default;

			~future_associated_state() noexcept {
				switch (m_state) {

				case future_result_state::RESULT: {
					m_result.result.~type();
					return;
				}

				case future_result_state::EXCEPTION: {
					m_result.exception.~exception_ptr();
					return;
				}

				}
			}

			template<class ... argument_types>
			void set_result(argument_types&& ... args) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				new (std::addressof(m_result.result)) type(std::forward<argument_types>(args)...);
				m_state = future_result_state::RESULT;
				call_continuation();
			}

			void set_exception(std::exception_ptr exception_pointer) {
				future_associated_state_base::set_exception(std::move(exception_pointer), std::addressof(m_result.exception));
			}

			type result_or_exception() {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return result_or_exception_unlocked();
			}

			type result_or_exception_unlocked() {
				assert(m_state != future_result_state::NOT_READY);

				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_result.exception);
				}

				return std::move(m_result.result);
			}

			type get() {
				wait();
				return result_or_exception_unlocked();
			}

		};

		template<>
		class future_associated_state<void> : public future_associated_state_base {

		private:
			std::exception_ptr m_exception;

		public:

			void set_exception(std::exception_ptr exception_pointer) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				m_exception = std::move(exception_pointer);
				m_state = future_result_state::EXCEPTION;
				call_continuation();
			}

			void set_result() {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				m_state = future_result_state::RESULT;
				call_continuation();
			}

			void result_or_exception() {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				result_or_exception_unlocked();
			}

			void result_or_exception_unlocked() {
				assert(m_state != future_result_state::NOT_READY);
				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_exception);
				}
			}

			void get() {
				wait();
				result_or_exception_unlocked();
			}

		};

		template<class type>
		class future_associated_state<type&> : public future_associated_state_base {

			compressed_future_result<type&> m_result;

		public:

			future_associated_state() noexcept = default;

			~future_associated_state() noexcept {
				if (m_state == future_result_state::EXCEPTION) {
					m_result.exception.~exception_ptr();
				}
			}

			void set_result(type& reference) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				m_result.result = std::addressof(reference);
				m_state = future_result_state::RESULT;
				call_continuation();
			}

			void set_exception(std::exception_ptr exception_pointer) {
				future_associated_state_base::set_exception(std::move(exception_pointer), std::addressof(m_result.exception));
			}

			type& result_or_exception() {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return result_or_exception_unlocked();
			}

			type& result_or_exception_unlocked() {
				assert(m_state != future_result_state::NOT_READY);

				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_result.exception);
				}

				assert(m_result.result != nullptr);
				return *m_result.result;
			}

			type& get() {
				wait();
				return result_or_exception_unlocked();
			}
		};

		template<class type>
		class future_awaiter {
		
		private:
			future_associated_state<type>& m_state;
			::concurrencpp::future<type>& m_future;

		public:
			future_awaiter(decltype(m_state) state, decltype(m_future) future) noexcept : m_state(state), m_future(future)  {}

			bool await_ready() const noexcept { return m_state.is_ready(); }
			type await_resume() { return m_state.get(); }

			template<class coroutine_handle>
			void await_suspend(coroutine_handle&& handle) {
				m_state.set_coro_handle(handle);
				auto local = std::move(m_future); //invalidate the future
			}
		};

		template<class type>
		class promise_base {

		protected:
			std::shared_ptr<details::future_associated_state<type>> m_state;
			bool m_future_retreived, m_fulfilled, m_moved;

			promise_base() noexcept : m_fulfilled(false), m_future_retreived(false), m_moved(false) {}
			promise_base(promise_base&& rhs) noexcept = default;
			promise_base& operator = (promise_base&& rhs) noexcept = default;

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
				if (static_cast<bool>(m_state) &&
					!m_fulfilled &&
					!m_state->has_deffered_task()) {
					m_state->set_exception(std::make_exception_ptr(std::future_error(std::future_errc::broken_promise)));
				}
			}

		public:

			~promise_base() noexcept { break_promise_if_needed(); }

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

			bool valid() const noexcept { return !m_moved; }
		};

		template<class T>
		auto& get_inner_state(T& state_holder) noexcept {
			return state_holder.m_state;
		}

		template<class function_type>
		class async_state : public callback_base, public pool_allocated {

			using return_type = typename std::result_of_t<function_type()>;

		private:
			::concurrencpp::promise<return_type> m_promise;
			function_type m_function;

		public:
			async_state(function_type&& function) : m_function(std::forward<function_type>(function)) {}

			auto get_future() { return m_promise.get_future(); }

			virtual void execute() override final { promise_setter::execute_set_promise(m_promise, m_function); }
		};

		template<class scheduler_type, class function_type>
		auto async_impl(function_type&& function) {
			auto task_ptr = new async_state<function_type>(std::forward<function_type>(function));
			std::unique_ptr<callback_base> task(task_ptr);
			auto future = task_ptr->get_future();
			auto future_state = get_inner_state(future);
			scheduler_type::schedule(std::move(task), future_state.get());
			return future;
		}

		struct thread_pool_scheduler {
			template<class type>
			static void schedule(std::unique_ptr<callback_base> task, future_associated_state<type>* state) {
				auto& thread_pool = ::concurrencpp::details::thread_pool::default_instance();
				thread_pool.enqueue_task(std::move(task));
			}
		};

		struct thread_scheduler {
			template<class type>
			static void schedule(std::unique_ptr<callback_base> task, future_associated_state<type>* state) {
				::std::thread execution_thread([task = std::move(task)]() mutable {
					task->execute();
				});

				execution_thread.detach();
			}
		};

		struct deffered_schedueler {
			template<class type>
			static void schedule(std::unique_ptr<callback_base> task, future_associated_state<type>* state) {
				state->set_deffered_task(std::move(task));
			}
		};

		template<class type>
		class promise_type_base {
	
		protected:
			::concurrencpp::promise<type> m_promise;

		public:		
			bool initial_suspend() const noexcept { return false; }
			bool final_suspend() const noexcept { return false; }
			::concurrencpp::future<type> get_return_object() { return m_promise.get_future(); }
			void set_exception(std::exception_ptr exception) { m_promise.set_exception(exception); }
		};

		class timer_impl {

		protected:
			size_t m_next_fire_time;
			std::atomic_size_t m_frequency;
			const size_t m_due_time;
			std::atomic_bool m_is_canceled;
			bool m_has_due_time_reached;
			const bool m_is_oneshot;

		public:

			enum class status {
				SCHEDULE,
				DELETE_,
				SCHEDULE_DELETE,
				IDLE
			};

			inline timer_impl(const size_t due_time, const size_t frequency, const bool is_oneshot) noexcept :
				m_due_time(due_time),
				m_next_fire_time(due_time),
				m_frequency(frequency),
				m_is_oneshot(is_oneshot),
				m_is_canceled(false),
				m_has_due_time_reached(false) {}

			virtual ~timer_impl() noexcept = default;

			inline status update(const size_t interval) noexcept {
				if (m_is_canceled.load(std::memory_order_acquire)) {
					return status::DELETE_;
				}

				if (!m_has_due_time_reached) {
					if (m_next_fire_time <= interval) {
						if (m_is_oneshot) {
							return status::SCHEDULE_DELETE;
						}

						//repeating timer:
						m_has_due_time_reached = true;
						m_next_fire_time = m_frequency.load(std::memory_order_acquire);
						return status::SCHEDULE;
					}
					else { //due time has not passed
						m_next_fire_time -= interval;
						return status::IDLE;
					}

				}
				else {	//frequency:
					if (m_next_fire_time <= interval) {
						m_next_fire_time = m_frequency.load(std::memory_order_acquire);
						return status::SCHEDULE;
					}
					else {
						m_next_fire_time -= interval;
						return status::IDLE;
					}
				}

				return status::IDLE;
			}

			inline void cancel() noexcept {
				m_is_canceled.store(true, std::memory_order_release);
			}

			inline const size_t next_fire_time() const noexcept {
				return m_next_fire_time;
			}

			inline void set_new_frequency_time(size_t new_frequency) noexcept {
				m_frequency.store(new_frequency, std::memory_order_release);
			}

			virtual void execute() = 0;

		};

		template<class task_type>
		class concrete_timer final : public timer_impl, public pool_allocated {

		private:
			task_type m_task;

		public:

			template<class function_type>
			concrete_timer(const size_t due_time, const size_t frequency, const bool is_oneshot, function_type&& function) :
				timer_impl(due_time, frequency, is_oneshot),
				m_task(std::forward<function_type>(function)) {}

			virtual void execute() override final { m_task(); }

		};

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

		class timer_queue {

			using timer_ptr = std::shared_ptr<timer_impl>;

		private:
			std::vector<timer_ptr> m_running_timers, m_queued_timers;
			std::mutex m_lock;
			std::condition_variable m_condition;
			std::chrono::system_clock::time_point m_last_time_point;
			thread_pool& m_thread_pool;
			bool m_is_running;

			inline void remove_timer(size_t index) {
				assert(index < m_running_timers.size());
				std::swap(m_running_timers.back(), m_running_timers[index]);
				m_running_timers.pop_back();
			}

			inline size_t add_queued_timers() {
				std::lock_guard<decltype(m_lock)> lock(m_lock);

				if (m_queued_timers.empty()) {
					return std::numeric_limits<size_t>::max();
				}

				auto comparator = [](const timer_ptr& timer_1, const timer_ptr& timer_2) {
					return timer_1->next_fire_time() < timer_2->next_fire_time();
				};

				const auto min_time = (**std::min_element(
					m_queued_timers.begin(),
					m_queued_timers.end(),
					comparator)).next_fire_time();

				m_running_timers.insert(
					m_running_timers.end(),
					std::make_move_iterator(m_queued_timers.begin()),
					std::make_move_iterator(m_queued_timers.end()));

				m_queued_timers.clear();
				return min_time;
			}

			inline size_t process_running_timers() {
				if (m_running_timers.empty()) {
					return std::numeric_limits<size_t>::max();
				}

				auto minimum_fire_time = m_running_timers[0]->next_fire_time();
				auto callback = [](timer_ptr timer) { timer->execute(); };

				for (size_t i = 0; i < m_running_timers.size(); i++) {
					auto& timer = m_running_timers[i];
					const auto chrono_interval =
						std::chrono::system_clock::now().time_since_epoch() -
						m_last_time_point.time_since_epoch();

					const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(chrono_interval).count();
					const auto status = timer->update(static_cast<size_t>(interval));
					const auto next_fire_time = timer->next_fire_time();
					if (next_fire_time < minimum_fire_time) {
						minimum_fire_time = next_fire_time;
					}

					switch (status) {

					case timer_impl::status::SCHEDULE: {
						m_thread_pool.enqueue_task(callback, timer);
						break;
					}

					case timer_impl::status::SCHEDULE_DELETE: {
						m_thread_pool.enqueue_task(callback, std::move(timer));
						remove_timer(i);
						--i;
						break;
					}

					case timer_impl::status::DELETE_: {
						remove_timer(i);
						--i;
						break;
					}

					//end of switch
					}

					//end of for loop
				}

				m_last_time_point = std::chrono::system_clock::now();
				return minimum_fire_time;
			}

		public:

			timer_queue() :
				m_thread_pool(thread_pool::default_instance()),
				m_is_running(true),
				m_last_time_point(std::chrono::system_clock::now()) {}

			void add_timer(timer_ptr timer) {
				{
					std::lock_guard<decltype(m_lock)> lock(m_lock);
					m_queued_timers.emplace_back(std::move(timer));
				}

				m_condition.notify_one();
			}

			void work_loop() {
				//predicate which returns ​false if the waiting should be continued. 
				auto pred = [this] {
					if (!m_is_running) {
						return true; //break the work loop
					}

					//if there are no queued timers, then go back to sleep.
					return !m_queued_timers.empty();
				};

				while (true) {
					auto next_fire_time = process_running_timers();
					next_fire_time = std::min(next_fire_time, add_queued_timers());

					next_fire_time =
						(next_fire_time == std::numeric_limits<size_t>::max()) ?
						size_t(1000) * 60 * 60 * 24 * 30 * 100 :
						next_fire_time;

					std::unique_lock<decltype(m_lock)> lock(m_lock);
					m_condition.wait_for(lock, std::chrono::milliseconds(next_fire_time), pred);
		
					if (!m_is_running) {
						return;
					}
				}
			}

			void stop() {
				{
					std::lock_guard<decltype(m_lock)> lock(m_lock);
					m_is_running = false;
				}

				m_condition.notify_one();
			}

		};

		class timer_queue_container {

		private:
			timer_queue m_timer_queue;
			std::thread m_timer_queue_thread;

		public:

			timer_queue_container() {
				m_timer_queue_thread = std::thread([this] {
					m_timer_queue.work_loop();
				});
			}

			~timer_queue_container() {
				m_timer_queue.stop();
				m_timer_queue_thread.join();
			}

			void add_timer(std::shared_ptr<timer_impl> timer_ptr) {
				m_timer_queue.add_timer(std::move(timer_ptr));
			}

			static timer_queue_container& default_instance() {
				static timer_queue_container s_timer_queue_container;
				return s_timer_queue_container;
			}

		};

	}

	template<class type>
	class future {

		template<class type> friend auto& details::get_inner_state(type& state_holder) noexcept;

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
		future& operator = (future&& rhds) noexcept = default;

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
			throw_if_empty();
			future _this(std::move(*this));
			return _this.m_state->get();
		}

		template<class continuation_type, class ret_type = typename std::result_of_t<continuation_type(future<type>)>>	
		future<ret_type> then(continuation_type&& continuation) {
			throw_if_empty();
			auto this_state = m_state.get();
			return this_state->set_then(std::move(*this), std::forward<continuation_type>(continuation));
		}

		auto operator co_await() {
			throw_if_empty();
			return details::future_awaiter<type>(*m_state, *this);
		}
	};

	template<class result_type, class ... argument_types>
	future<result_type> make_ready_future(argument_types&& ... args) {
		promise<result_type> promise;
		promise.set_value(std::forward<argument_types>(args)...);
		return promise.get_future();
	}

	template<class type = void>
	future<void> make_ready_future() {
		promise<void> promise;
		promise.set_value();
		return promise.get_future();
	}
	
	template<class result_type, class exception_type, class ... argument_types>
	future<result_type> make_exceptional_future(argument_types&& ... args) {
		return make_exceptional_future<result_type>(
			std::make_exception_ptr(exception_type(std::forward<argument_types>(args)...)));
	}

	template<class result_type>
	future<result_type> make_exceptional_future(std::exception_ptr exception_pointer) {
		promise<result_type> promise;
		promise.set_exception(exception_pointer);
		return promise.get_future();
	}

	template<class type>
	struct promise : public details::promise_base<type> {

		promise() noexcept = default;
		
		promise(promise&& rhs) noexcept {
			m_state = std::move(rhs.m_state);
			rhs.m_moved = true;
		}
		
		promise& operator = (promise&& rhs) noexcept {
			break_promise_if_needed();
			
			m_state = std::move(rhs.m_state);
			m_moved = rhs.m_moved;
			m_fulfilled = rhs.m_fulfilled;
			m_future_retreived = rhs.m_future_retreived;
			
			rhs.m_moved = true;
			return *this;
		}

		template<class ... argument_types>
		void set_value(argument_types&& ... args) {
			ensure_state();
			m_state->set_result(std::forward<argument_types>(args)...);
			m_fulfilled = true;
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

		promise(promise&& rhs) noexcept {
			m_state = std::move(rhs.m_state);
			rhs.m_moved = true;
		}

		promise& operator = (promise&& rhs) noexcept {
			break_promise_if_needed();
			m_state = std::move(rhs.m_state);
			m_moved = rhs.m_moved;
			m_fulfilled = rhs.m_fulfilled;
			m_future_retreived = rhs.m_future_retreived;
			rhs.m_moved = true;
			return *this;
		}

		void set_value() {
			ensure_state();
			m_state->set_result();
			m_fulfilled = true;
		}
	};

	template<class function, class ... arguments>
	void spawn(function&& func, arguments&& ... args) {
		details::thread_pool::default_instance().enqueue_task(
			std::forward<function>(func),
			std::forward<arguments>(args)...);
	}

	template<class function_type>
	void spawn(function_type&& function) {
		details::thread_pool::default_instance().enqueue_task(std::forward<function_type>(function));
	}

	enum class launch {
		async,
		deferred,
		task
	};

	template <class F, class... Args>
	auto async(launch launch_policy, F&& f, Args&&... args) {
		using function_type = typename std::decay_t<F>;
		using result = typename std::result_of_t<function_type(Args...)>;

		switch (launch_policy) {

		case launch::task: {
			return details::async_impl<result, details::thread_pool_scheduler>::do_async(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
		}

		case launch::deferred: {
			return details::async_impl<result, details::deffered_schedueler>::do_async(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
		}

		case launch::async: {
			return details::async_impl<result, details::thread_scheduler>::do_async(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
		}

		}

		assert(false);
		return decltype(details::async_impl<
			result, details::thread_pool_scheduler>::do_async(
				std::bind(std::forward<F>(f), std::forward<Args>(args)...))){};
	}

	template <class function_type>
	auto async(launch launch_policy, function_type&& function) {
	
		switch (launch_policy) {

		case ::concurrencpp::launch::task: {
			return details::async_impl<details::thread_pool_scheduler>(
				std::forward<function_type>(function));
		}
		case ::concurrencpp::launch::async: {
			return details::async_impl<details::thread_scheduler>(
				std::forward<function_type>(function));
		}

		case ::concurrencpp::launch::deferred: {
			return details::async_impl<details::deffered_schedueler>(
				std::forward<function_type>(function));
		}

		}

		assert(false);
		return details::async_impl<details::deffered_schedueler>(
			std::forward<function_type>(function));
	}

	template <class function_type, class... argument_types>
	auto async(function_type&& function, argument_types&&... arguments) {
		return async(
			launch::task,
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
			::concurrencpp::promise<decltype(m_tuple)> m_promise;

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
				future.then([_this = shared_from_this()](::concurrencpp::future<type> done_future){
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

			auto get_future() { return m_promise.get_future(); }
		};

		struct when_any_state {
			::std::atomic_bool fulfilled;
			::concurrencpp::promise<size_t> promise;

			when_any_state() noexcept : fulfilled(false) {}

			void on_task_finished(size_t index) {
				const auto _fulfilled = std::atomic_exchange_explicit(
					&fulfilled,
					true,
					std::memory_order_acquire);

				if (_fulfilled == false) { //this is the first finished task
					promise.set_value(index);
				}
			}
		};

		inline void when_any_once(const std::shared_ptr<when_any_state>& state, size_t task_index) noexcept {}

		template<class type, class ... types>
		void when_any_once(
			std::shared_ptr<when_any_state> state,
			size_t task_index,
			::concurrencpp::future<type>& future,
			types&& ... future_types) {

			if (!future.valid()) {
				throw std::future_error(std::future_errc::no_state);
			}

			if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
				state->on_task_finished(task_index);
				return;
			}

			auto& inner_state = get_inner_state(future);
			inner_state->wrap_continuation([state = state, task_index] {
				state->on_task_finished(task_index);
			});

			when_any_once(std::move(state), task_index + 1, std::forward<types>(future_types)...);
		}

		template<class ... types>
		future<size_t> when_any_wrapper(types&& ... future_types) {
			pool_allocator<when_any_state> allocator;
			auto state = std::allocate_shared<when_any_state>(allocator);
			details::when_any_once(state, 0, std::forward<types>(future_types)...);
			return state->promise.get_future();
		}
	}

	template<class ... future_types>
	future<std::tuple<typename std::decay<future_types>::type...>>
		when_all(future_types&& ... futures) {
		auto when_all_state = 
			details::make_shared<details::when_all_state<typename std::decay<future_types>::type...>>();
		when_all_state->set_futures(std::forward<future_types>(futures)...);
		return when_all_state->get_future();
	}

	template<class ... future_types>
	future<size_t> when_any(future_types&& ... futures) {
		return details::when_any_wrapper(std::forward<future_types>(futures)...);
	}

	class timer {

	private:
		std::shared_ptr<details::timer_impl> m_impl;

		void throw_if_empty() const {
			if (static_cast<bool>(m_impl)) {
				return;
			}

			throw empty_timer("concurrencpp::timer - timer is empty.");
		}

		template<class function_type, class ... arguments_type>
		static void init_timer(
			std::shared_ptr<details::timer_impl>& timer_ptr,
			size_t due_time,
			size_t frequency,
			bool is_oneshot,
			function_type&& function,
			arguments_type&& ... args) {

			auto& timer_queue_container = details::timer_queue_container::default_instance();

			timer_ptr = details::make_timer_impl(
				due_time,
				frequency,
				is_oneshot,
				std::forward<function_type>(function),
				std::forward<arguments_type>(args)...);

			timer_queue_container.add_timer(timer_ptr);
		}

	public:

		template<class function_type, class ... arguments_type>
		timer(size_t due_time, size_t frequency, function_type&& function, arguments_type&& ... args) {
			init_timer(
				m_impl,
				due_time,
				frequency,
				false,
				std::forward<function_type>(function),
				std::forward<arguments_type>(args)...);
		}

		timer(const timer&) = delete;
		timer(timer&& rhs) noexcept = default;

		timer& operator = (const timer&) = delete;
		timer& operator = (timer&& rhs) noexcept = default;

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

		struct empty_timer : public std::runtime_error {

			template<class ... arguments>
			empty_timer(arguments&& ... args) : std::runtime_error(std::forward<arguments>(args)...) {}

		};

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
			::concurrencpp::promise<void> promise;
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
				void return_value(return_type&& value) {
					m_promise.set_value(std::forward<return_type>(value));
				}

			};
		};

		template<class... arguments>
		struct coroutine_traits<::concurrencpp::future<void>, arguments...> {

			struct promise_type :
				public concurrencpp::details::promise_type_base<void> ,
				public concurrencpp::details::pool_allocated {

				void return_void() {
					m_promise.set_value();
				}
			};
		};

		template<class ... arguments>
		struct coroutine_traits<void, arguments...> {

			struct promise_type : public concurrencpp::details::pool_allocated {
				promise_type() noexcept {}
				void get_return_object() noexcept {}
				bool initial_suspend() const noexcept { return false; }
				bool final_suspend() const noexcept { return false; }
				void return_void() noexcept {}

				template<class exception_type>
				void set_exception(exception_type&& exception) noexcept {}

			};
		};

	}//experimental
}//std

#endif