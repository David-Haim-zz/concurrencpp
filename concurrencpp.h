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

			void unlock() noexcept {
				m_lock.store(unlocked, std::memory_order_release);
			}

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

			size_t get_block_count() const noexcept {
				return m_block_count;
			}

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
				auto number_of_cpus = std::thread::hardware_concurrency();
				if (number_of_cpus == 0) {
					number_of_cpus = DEFAULT_POOL_SIZE;
				}

				return number_of_cpus;
			}
			
			memory_pool(): m_pools(calculate_pool_size()){}

			~memory_pool() {
				for (auto& pool : m_pools) {
					for (auto& bucket : pool) {
						std::lock_guard<decltype(bucket.second)> lock(bucket.second);
						bucket.first.clear();
					}
				}
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

		template <class T>
		struct pool_allocator {

			typedef T value_type;

			pool_allocator() noexcept {}

			template<class U>
			pool_allocator(const pool_allocator<U>&) noexcept {}

			template<class U>
			bool operator == (const pool_allocator<U>&) const noexcept {
				return true;
			}

			template<class U>
			bool operator != (const pool_allocator<U>&) const noexcept {
				return false;
			}

			T* allocate(const size_t n) const {
				if (n == 0) {
					return nullptr;
				}

				return static_cast<T*>(memory_pool::allocate(n * sizeof(T)));
			}

			void deallocate(T* const block, const size_t size) const noexcept {
				memory_pool::deallocate(block, size * sizeof(T));
			}

		};

		struct pool_allocated {
			static void* operator new(const size_t size) {
				return memory_pool::allocate(size);
			}

			static void operator delete(void* block, size_t size) {
				memory_pool::deallocate(block, size);
			}

		};
	}

	template<class T> class future;
	template<class T> class promise;

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

			static size_t number_of_threads_cpu() {
				const auto concurrency_level = std::thread::hardware_concurrency();
				return concurrency_level == 0 ? 8 : static_cast<size_t>(concurrency_level * 1.25f);
			}

			static size_t number_of_threads_io() {
				const auto concurrency_level = std::thread::hardware_concurrency();
				return concurrency_level == 0 ? 8 : (concurrency_level * 2);
			}

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
				enqueue_task(std::bind(
					std::forward<function_type>(function),
					std::forward<arguments>(args)...));
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

		class future_associated_state_base {

		protected:
			//members are ordered in the order of their importance.
			mutable recursive_spinlock m_lock;
			std::unique_ptr<callback_base> m_then;
			future_result_state m_state;
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

				return has_result ? std::future_status::ready :
					std::future_status::timeout;
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
						auto deffered_task = std::move(m_then);
						m_then = std::move(deffered_task->next);
						deffered_task->execute();
						deffered_task.reset();
						call_continuation();
						assert(m_state != future_result_state::DEFFERED);
						assert(m_state != future_result_state::NOT_READY);
						return;
					}
				}

				while (wait_for(std::chrono::hours(365 * 24)) == std::future_status::timeout);
			}

			void set_continuation(std::unique_ptr<callback_base> continuation) {
				{
					std::unique_lock<decltype(m_lock)> lock(m_lock);

					if (m_state == future_result_state::DEFFERED) {
						assert(m_then->next.get() == nullptr);
						m_then->next = std::move(continuation);
						return;
					}

					if (m_state == future_result_state::NOT_READY) {
						assert(!static_cast<bool>(m_then));
						m_then = std::move(continuation);
						return;
					}
				}

				//the future is ready and synchronized (by locking the lock), just call the continuation.
				//no promise can touch this future again.
				continuation->execute();
			}

			bool has_deffered_task() const noexcept {
				return m_state == future_result_state::DEFFERED;
			}

			template<class function_type>
			void wrap_continuation(function_type&& callback) noexcept {
				std::unique_lock<decltype(m_lock)> lock(m_lock);

				if (m_state != future_result_state::NOT_READY) {
					//nothing to wrap, just call the callback
					callback();
					return;
				}

				std::unique_ptr<callback_base> new_then;
				auto then = std::move(m_then);

				if (static_cast<bool>(then)) {
					new_then = make_callback([
						then = std::move(then),
							callback = std::forward<function_type>(callback)]() mutable{
							callback();
							then->execute();
						});
				}
				else {
					new_then = make_callback([
						callback = std::forward<function_type>(callback)]() mutable{
							callback();
						});
				}

				assert(static_cast<bool>(new_then));
				assert(!static_cast<bool>(m_then));
				m_then = std::move(new_then);
			}

			bool is_ready() const noexcept {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return m_state == future_result_state::EXCEPTION ||
					m_state == future_result_state::RESULT;
			}

			void call_continuation() {
				if (static_cast<bool>(m_condition)) {
					m_condition->notify_all();
					//do not reset the CV, as other objects wait on it on another thread.
				}

				if (static_cast<bool>(m_then)) {
					m_then->execute();
					m_then.reset();
				}
			}

			void set_deffered_task(std::unique_ptr<callback_base> task) noexcept {
				/*
					this function should only be called once,
					by using async + launch::deffered
				*/
				std::unique_lock<decltype(m_lock)> lock(m_lock);

				assert(m_state == future_result_state::NOT_READY);
				assert(!static_cast<bool>(m_then));

				m_then = std::move(task);
				m_state = future_result_state::DEFFERED;
			}

			bool has_continuation() const noexcept {
				return static_cast<bool>(m_then);
			}
		};

		template<class T>
		class future_associated_state :
			public future_associated_state_base {

		private:
			compressed_future_result<T> m_result;

		public:

			future_associated_state() noexcept = default;

			~future_associated_state() noexcept {
				switch (m_state) {

				case future_result_state::RESULT: {
					m_result.result.~T();
					return;
				}

				case future_result_state::EXCEPTION: {
					m_result.exception.~exception_ptr();
					return;
				}

				}
			}

			template<class ... arguments>
			void set_result(arguments&& ... args) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				new (std::addressof(m_result.result))
					T(std::forward<arguments>(args)...);
				m_state = future_result_state::RESULT;
				call_continuation();
			}

			template<class exception>
			void set_exception(exception&& given_exception) {
				set_exception(std::make_exception_ptr(std::forward<exception>(given_exception)));
			}

			void set_exception(std::exception_ptr exception_pointer) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				new (std::addressof(m_result.exception))
					std::exception_ptr(std::move(exception_pointer));
				m_state = future_result_state::EXCEPTION;
				call_continuation();
			}

			T result_or_exception() {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return result_or_exception_unlocked();
			}

			T result_or_exception_unlocked() {
				assert(m_state != future_result_state::NOT_READY);

				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_result.exception);
				}

				return std::move(m_result.result);
			}

			T get() {
				wait();
				return result_or_exception_unlocked();
			}

		};

		template<>
		class future_associated_state<void> :
			public future_associated_state_base {

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

		template<class T>
		class future_associated_state<T&> :
			public future_associated_state_base {

			compressed_future_result<T&> m_result;

		public:

			future_associated_state() noexcept = default;

			~future_associated_state() noexcept {
				if (m_state == future_result_state::EXCEPTION) {
					m_result.exception.~exception_ptr();
				}
			}

			void set_result(T& reference) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				m_result.result = std::addressof(reference);
				m_state = future_result_state::RESULT;
				call_continuation();
			}

			template<class exception>
			void set_exception(exception&& given_exception) {
				set_exception(std::make_exception_ptr(std::forward<exception>(given_exception)));
			}

			void set_exception(std::exception_ptr exception_pointer) {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY || m_state == future_result_state::DEFFERED);
				new (std::addressof(m_result.exception))
					std::exception_ptr(std::move(exception_pointer));
				m_state = future_result_state::EXCEPTION;
				call_continuation();
			}

			T& result_or_exception() {
				std::unique_lock<decltype(m_lock)> lock(m_lock);
				return result_or_exception_unlocked();
			}

			T& result_or_exception_unlocked() {
				assert(m_state != future_result_state::NOT_READY);

				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_result.exception);
				}

				assert(m_result.result != nullptr);
				return *m_result.result;
			}

			T& get() {
				wait();
				return result_or_exception_unlocked();
			}
		};

		template<class original_type, class callback_type>
		class future_then : public callback_base, public pool_allocated {

			using new_type = typename std::result_of_t<callback_type(::concurrencpp::future<original_type>)>;
			using future_type = ::concurrencpp::future<original_type>;
			using is_void = typename std::conditional_t<
				std::is_same<new_type, void>::value,
				std::true_type,
				std::false_type>;

		private:
			::concurrencpp::promise<new_type> m_promise;
			std::shared_ptr<future_associated_state<original_type>> m_future_state;
			callback_type m_callback;

			void execute_impl(std::true_type) {
				m_callback(future_type(std::move(m_future_state)));
				m_promise.set_value();
			}

			void execute_impl(std::false_type) {
				m_promise.set_value(m_callback(future_type(std::move(m_future_state))));
			}

		public:
			
			future_then(decltype(m_future_state) state, callback_type&& callback) :
				m_future_state(std::move(state)),
				m_callback(std::forward<callback_type>(callback)) {}

			virtual void execute() override final {
				try {
					execute_impl(is_void{});
				}
				catch (...) {
					m_promise.set_exception(std::current_exception());
				}
			}

			auto get_future() {
				return m_promise.get_future();
			}

		};

		template<class future_type, class callback_type>
		auto make_future_then(
			std::shared_ptr<future_associated_state<future_type>> future_state,
			callback_type&& callback) {
			auto then_ptr = new future_then<future_type, callback_type>{
				future_state,
				std::forward<callback_type>(callback)
			};

			std::unique_ptr<callback_base> type_erased(then_ptr);
			auto new_future = then_ptr->get_future();

			assert(future_state.get() != nullptr); //should have been passed by value.
			future_state->set_continuation(std::move(type_erased));

			return new_future;
		}

		template<class type>
		class future_awaiter {
		
		private:
			future_associated_state<type>& m_state;

		public:
			future_awaiter(decltype(m_state) state) noexcept : m_state(state) {}

			bool await_ready() noexcept { return m_state.is_ready(); }

			template<class coroutine_handle>
			void await_suspend(coroutine_handle&& handle) {
				m_state.set_continuation(make_callback(std::forward<coroutine_handle>(handle)));
			}

			type await_resume() { return m_state.get(); }
		};

		class promise_base {

		protected:
			bool m_future_retreived, m_fulfilled, m_moved;

			promise_base() noexcept : m_fulfilled(false), m_future_retreived(false), m_moved(false) {}
			promise_base(promise_base&& rhs) noexcept = default;
			promise_base& operator = (promise_base&& rhs) noexcept = default;

		public:

			bool valid() const noexcept {
				return !m_moved;
			}

		};

		template<class T>
		auto& get_inner_state(T& state_holder) noexcept {
			return state_holder.m_state;
		}

		template<class function_type>
		class async_state : public callback_base, public pool_allocated {

			using return_type = typename std::result_of_t<function_type()>;
			using is_void_type = typename std::conditional_t<
				std::is_same_v<return_type, void>,
				std::true_type,
				std::false_type>;

			void execute_impl(std::false_type) {
				m_promise.set_value(m_function());
			}

			void execute_impl(std::true_type) {
				m_function();
				m_promise.set_value();
			}

		private:
			::concurrencpp::promise<return_type> m_promise;
			function_type m_function;

		public:
			async_state(function_type&& function) :
				m_function(std::forward<function_type>(function)) {}

			auto get_future() {
				return m_promise.get_future();
			}

			virtual void execute() override final {
				try {
					execute_impl(is_void_type{});
				}
				catch (...) {
					m_promise.set_exception(std::current_exception());
				}
			}
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
			template<class T>
			static void schedule(std::unique_ptr<callback_base> task, future_associated_state<T>* state) {
				auto& thread_pool = ::concurrencpp::details::thread_pool::default_instance();
				thread_pool.enqueue_task(std::move(task));
			}
		};

		struct thread_scheduler {
			template<class T>
			static void schedule(std::unique_ptr<callback_base> task, future_associated_state<T>* state) {
				::std::thread execution_thread([_task = std::move(task)]() mutable {
					_task->execute();
				});

				execution_thread.detach();
			}
		};

		struct deffered_schedueler {
			template<class T>
			static void schedule(std::unique_ptr<callback_base> task, future_associated_state<T>* state) {
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

			::concurrencpp::future<type> get_return_object() {
				return m_promise.get_future();
			}

			void set_exception(std::exception_ptr exception) {
				m_promise.set_exception(exception);
			}

			template<class exception_type>
			void set_exception(exception_type&& exception) {
				m_promise.set_exception(std::forward<exception_type>(exception));
			}

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

		public:

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

			inline bool cancelled() noexcept {
				return m_is_canceled.load(std::memory_order_acquire);
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
		class concrete_timer final : public timer_impl {

		private:
			task_type m_task;

		public:

			template<class function_type>
			concrete_timer(const size_t due_time, const size_t frequency, const bool is_oneshot, function_type&& function) :
				timer_impl(due_time, frequency, is_oneshot),
				m_task(std::forward<function_type>(function)) {}

			virtual void execute() override final {
				m_task();
			}

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

		template<class function_type, class ... arguments>
		std::shared_ptr<timer_impl> make_timer_impl(
			const size_t due_time,
			const size_t frequency,
			const bool is_oneshot,
			function_type&& function,
			arguments&& ... args) {

			return make_timer_impl(
				due_time,
				frequency,
				is_oneshot, 
				std::bind(std::forward<function_type>(function),
					std::forward<arguments>(args)...));
		}

		class timer_queue {

			using timer_ptr = std::shared_ptr<timer_impl>;

		private:
			std::vector<timer_ptr> m_running_timers, m_queued_timers;
			std::mutex m_lock;
			std::condition_variable m_condition;
			std::chrono::system_clock::time_point m_last_time_point;
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
				auto& thread_pool = thread_pool::default_instance();
				auto callback = [](timer_ptr timer) {
					timer->execute();
				};

				for (auto i = 0ul; i < m_running_timers.size(); i++) {
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
						thread_pool.enqueue_task(callback, timer);
						break;
					}

					case timer_impl::status::SCHEDULE_DELETE: {
						thread_pool.enqueue_task(callback, timer);
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
				while (true) {
					auto next_fire_time = process_running_timers();
					next_fire_time = std::min(next_fire_time, add_queued_timers());

					auto pred = [this] {
						if (!m_is_running) {
							return true; //break the work loop
						}

						//if there are no queued timers, then go back to sleep.
						return !m_queued_timers.empty();
					};

					next_fire_time =
						(next_fire_time == std::numeric_limits<size_t>::max()) ?
						size_t(1000) * 60 * 60 * 24 * 30 * 100 :
						next_fire_time;

					std::unique_lock<decltype(m_lock)> lock(m_lock);
					m_condition.wait_for(
						lock,
						std::chrono::milliseconds(next_fire_time),
						pred);

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

		template<class type> friend class ::concurrencpp::details::future_associated_state;
		template<class type> friend class ::concurrencpp::promise;
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

		bool valid() const noexcept {
			return static_cast<bool>(m_state);
		}

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

		template<class continuation_type>
		auto then(continuation_type&& continuation) {
			throw_if_empty();
			return make_future_then(m_state, std::forward<continuation_type>(continuation));
		}

		auto operator co_await() {
			throw_if_empty();
			return details::future_awaiter<type>(*m_state);
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

	template<class T>
	class promise : public details::promise_base {

	private:
		std::shared_ptr<details::future_associated_state<T>> m_state;

		void create_state() {
			details::pool_allocator<details::future_associated_state<T>> allocator;
			m_state = std::allocate_shared<details::future_associated_state<T>>(allocator);
		}

		void ensure_state() {
			if (m_moved) {
				throw std::future_error(std::future_errc::no_state);
			}

			if (m_fulfilled) {
				throw std::future_error(std::future_errc::promise_already_satisfied);
			}

			if (!m_state) {
				create_state();
			}
		}

		void break_promise_if_needed() {
			if (static_cast<bool>(m_state) &&
				!m_fulfilled &&
				!m_state->has_deffered_task()) {
				m_state->set_exception(std::future_error(std::future_errc::broken_promise));
			}
		}

	public:

		promise() noexcept = default;
		
		promise(promise&& rhs) noexcept {
			m_state = std::move(rhs.m_state);
			rhs.m_moved = true;
		}
		
		promise& operator = (promise&& rhs) noexcept {
			break_promise_if_needed();
			m_state = std::move(rhs.m_state);
			m_moved = rhs.m_moved;
			rhs.m_moved = true;
			return *this;
		}

		~promise() noexcept {
			break_promise_if_needed();
		}

		future<T> get_future() {
			if (m_moved) {
				throw std::future_error(std::future_errc::no_state);
			}

			if (m_future_retreived) {
				throw std::future_error(std::future_errc::future_already_retrieved);
			}

			if (!static_cast<bool>(m_state)) {
				create_state();
			}

			m_future_retreived = true;
			return future<T>(m_state);
		}

		template<class ... arguments>
		void set_value(arguments&& ... args) {
			ensure_state();
			m_state->set_result(std::forward<arguments>(args)...);
			m_fulfilled = true;
		}

		void set_exception(std::exception_ptr exception_pointer) {
			ensure_state();
			m_state->set_exception(exception_pointer);
			m_fulfilled = true;
		}

		void swap(promise& rhs) noexcept {
			auto temp = std::move(rhs);
			rhs = std::move(*this);
			*this = std::move(temp);
		}

	};

	template<>
	class promise<void> : public details::promise_base {

	private:
		std::shared_ptr<details::future_associated_state<void>> m_state;

		void create_state() {
			details::pool_allocator<details::future_associated_state<void>> allocator;
			m_state = std::allocate_shared<details::future_associated_state<void>>(allocator);
		}

		void ensure_state() {
			if (m_moved) {
				throw std::future_error(std::future_errc::no_state);
			}

			if (m_fulfilled) {
				throw std::future_error(std::future_errc::promise_already_satisfied);
			}

			if (!m_state) {
				create_state();
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
		promise() noexcept = default;

		promise(promise&& rhs) noexcept {
			m_state = std::move(rhs.m_state);
			rhs.m_moved = true;
		}

		promise& operator = (promise&& rhs) noexcept {
			break_promise_if_needed();
			m_state = std::move(rhs.m_state);
			m_moved = rhs.m_moved;
			rhs.m_moved = true;
			return *this;
		}

		~promise() noexcept {
			break_promise_if_needed();
		}

		future<void> get_future() {
			if (m_moved) {
				throw std::future_error(std::future_errc::no_state);
			}

			if (m_future_retreived) {
				throw std::future_error(std::future_errc::future_already_retrieved);
			}

			if (!static_cast<bool>(m_state)) {
				create_state();
			}

			m_future_retreived = true;
			return future<void>(m_state);
		}

		void set_value() {
			ensure_state();
			m_state->set_result();
			m_fulfilled = true;
		}

		void set_exception(std::exception_ptr exception_pointer) {
			ensure_state();
			m_state->set_exception(exception_pointer);
			m_fulfilled = true;
		}

	};

	template<class function, class ... arguments>
	void spawn(function&& func, arguments&& ... args) {
		details::thread_pool::default_instance().enqueue_task(
			std::forward<function>(func),
			std::forward<arguments>(args)...);
	}

	template<class function>
	void spawn(function&& func) {
		details::thread_pool::default_instance().
			enqueue_task(std::forward<function>(func));
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
	auto async(function_type&& function) {
		return async(launch::task, std::forward<function_type>(function));
	}
}

namespace concurrencpp {

	namespace details {

		class when_all_state {

		private:
			concurrencpp::promise<void> m_promise;
			std::atomic_size_t m_counter;

		public:
			when_all_state(size_t counter) noexcept : m_counter(counter) {}

			void on_task_finished() {
				auto new_count = m_counter.fetch_sub(1, std::memory_order_acq_rel);
				if (new_count == 0) {
					m_promise.set_value();
				}
			}

			future<void> future() {
				return m_promise.get_future();
			}
		};

		inline void when_all_once(const std::shared_ptr<when_all_state>& state) noexcept {}

		template<class type, class ... types>
		void when_all_once(std::shared_ptr<when_all_state> state, ::concurrencpp::future<type>& future, types&& ... future_types) {
			if (!future.valid()) {
				throw std::future_error(std::future_errc::no_state);
			}

			auto& inner_state = get_inner_state(future);
			inner_state->wrap_continuation([state = state] {
				state->on_task_finished();
			});

			when_all_once(std::move(state), std::forward<types>(future_types)...);
		}

		template<class ... future_types>
		future<void> when_all_wrapper(future_types&& ... futures) {
			pool_allocator<when_all_state> allocator;
			auto state = std::allocate_shared<when_all_state>(allocator);
			when_all_once(state, std::forward<future_types>(futures)...);
			return state->future();
		}

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
	future<void> when_all(future_types&& ... futures) {
		return details::when_all_wrapper(std::forward<future_types>(futures)...);
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

			auto& timer_queue_container =
				details::timer_queue_container::default_instance();

			/*
				if for some reason we fail to build a threadpool
				let the exception be thrown synchronously on the caller thread.
			*/
			auto& thread_pool = details::thread_pool::default_instance();

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
		timer(timer&&) noexcept = default;

		timer& operator = (const timer&) = delete;
		timer& operator = (timer&&) noexcept = default;

		void cancel() {
			throw_if_empty();
			m_impl->cancel();
		}

		bool cancelled() const {
			throw_if_empty();
			return m_impl->cancelled();
		}

		void set_frequency(size_t new_frequency) {
			throw_if_empty();
			m_impl->set_new_frequency_time(new_frequency);
		}

		bool valid() const noexcept {
			return static_cast<bool>(m_impl);
		}

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

			struct delayer {

				size_t due_time;

				delayer(size_t due_time) noexcept : due_time(due_time) {}

				bool await_ready() const noexcept { return false; }

				void await_suspend(std::experimental::coroutine_handle<void> coro_handle) const {
					timer::once(due_time, coro_handle);
				}

				void await_resume() const noexcept {}

			};

			delayer delayer(due_time);
			co_await delayer;
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