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

namespace concurrencpp { 
	namespace details {

		class spinlock {
			constexpr static size_t locked = 1;
			constexpr static size_t unlocked = 0;
			constexpr static size_t spinCount = 128;

		private:
			std::atomic_size_t m_lock;

		public:

			spinlock() noexcept : m_lock(unlocked) {}

			void lock() noexcept {
				size_t counter = 0ul;

				//make the processor yield
				while (true) {
					const auto state = std::atomic_exchange_explicit(
						&m_lock,
						locked,
						std::memory_order_acquire);

					if (state == unlocked) {
						return;
					}

					if (counter == spinCount) {
						break;
					}

					++counter;
					//TODO: make a cross platform way to call _mm_pause
				}

				counter = 0ul;

				//make the thread yield
				while (true) {
					const auto state = std::atomic_exchange_explicit(
						&m_lock,
						locked,
						std::memory_order_acquire);

					if (state == unlocked) {
						return;
					}

					if (counter == spinCount) {
						break;
					}

					++counter;
					std::this_thread::yield();
				}

				//make the thread sleep for 1 millisecond
				while (std::atomic_exchange_explicit(
					&m_lock,
					locked,
					std::memory_order_acquire) == locked) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
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

		struct memory_block {
			memory_block* next;
		};

		class block_list {
			
		private:
			memory_block* m_head;
			size_t m_block_count;

		public:

			block_list() noexcept : m_head(nullptr), m_block_count(0ul) {}
			
			~block_list() noexcept {
				auto cursor = m_head;
				while (cursor != nullptr) {
					auto temp = cursor;
					cursor = cursor->next;
					std::free(temp);
				}
			}

			void* allocate() noexcept {
				if (m_head == nullptr) {
					return nullptr;
				}

				auto block = m_head;
				m_head = m_head->next;
				--m_block_count;
				return block;
			}
		
			void deallocate(void* chunk) {
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

		private:

			using synchronized_list_type = std::pair<block_list, spinlock>;
			using local_pool_type = std::array<block_list, 8>;
			using global_pool_type = std::array<synchronized_list_type, 8>;
			//pool = [32, 64 , 96, 128, 192, 256, 384, 512]

			static constexpr size_t MAX_BLOCK_COUNT = 1024 * 64;

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

			static global_pool_type& get_global_pool() noexcept {
				static global_pool_type s_global_pool;
				return s_global_pool;
			}

			static local_pool_type& get_local_pool() noexcept {
				static thread_local local_pool_type s_local_pool;
				return s_local_pool;
			}

			static void* allocate_from_global_pool(const size_t bucket_index) {
				auto& global_pool = get_global_pool();
				assert(bucket_index < global_pool.size());
				auto& bucket = global_pool[bucket_index];

				std::lock_guard<spinlock> lock(bucket.second);
				return bucket.first.allocate();
			}

			static bool deallocate_to_global_pool(void* block, size_t index) {
				auto& global_pool = get_global_pool();
				assert(index < global_pool.size());
			
				auto& synchonized_bucket = global_pool[index];
				std::lock_guard<spinlock> lock(synchonized_bucket.second);
				const auto count = synchonized_bucket.first.get_block_count();
				if (count < MAX_BLOCK_COUNT) {
					synchonized_bucket.first.deallocate(block);
					return true;
				}

				return false;
			}

			static void* allocate_imp(size_t unaligned_size) noexcept {
				/*
					size == 0
				*/
				
				const auto index = find_bucket_index(unaligned_size);
				auto& local_pool = get_local_pool();
				auto block = local_pool[index].allocate();
				if (block != nullptr) {
					return block;
				}

				block = allocate_from_global_pool(index);
				if (block != nullptr) {
					return block;
				}

				return std::malloc(align_size(unaligned_size));
			}

			static void deallocate_impl(void* block, size_t unaligned_size) noexcept {
				if (unaligned_size > 512) {
					std::free(block);
					return;
				}
				
				const auto index = find_bucket_index(unaligned_size);
				auto& local_pool = get_local_pool();
				assert(index < local_pool.size());

				const auto count = local_pool[index].get_block_count();
				if (count < MAX_BLOCK_COUNT) {
					local_pool[index].deallocate(block);
					return;
				}

				if (!deallocate_to_global_pool(block, index)) {
					std::free(block);
				}
			}

		public:

			static void* allocate(size_t size) noexcept {
				return allocate_imp(size);
			}

			static void deallocate(void* chunk, size_t size) noexcept {
				deallocate_impl(chunk, size);
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
				if (n == 0){
					return nullptr;
				}
				
				void* const pv = memory_pool::allocate(n * sizeof(T));
				
				if (!pv) { 
					throw std::bad_alloc(); 
				}
				
				return static_cast<T*>(pv);
			}
			
			void deallocate(T* const p, size_t size) const noexcept {
				memory_pool::deallocate(p, size * sizeof(T));
			}
		
		};
	}
}

namespace concurrencpp {

	template<class T> class future;
	template<class T> class promise;

	namespace details {

		struct callback_base {
			virtual ~callback_base() = default;
			virtual void execute() = 0;
		};

		template<class function_type>
		struct callback : public callback_base {

		private:
			function_type m_function;

		public:

			callback(function_type&& function) :
				m_function(std::forward<function_type>(function)) {}

			virtual void execute() override final {
				m_function();
			}

			static void* operator new(size_t size) {
				return memory_pool::allocate(size);
			}

			static void operator delete(void* block){
				memory_pool::deallocate(block, sizeof(callback<function_type>));
			}

		};

		template<class f>
		std::unique_ptr<callback_base> make_callback(f&& function) {
			return std::unique_ptr<callback_base>(
				new callback<f>(std::forward<f>(function)));
		}

		class worker_thread {

		private:
			spinlock m_lock;
			std::queue<std::unique_ptr<callback_base>> m_tasks;
			std::vector<worker_thread>& m_thread_group;
			std::atomic_bool& m_stop;
			std::condition_variable_any m_condition;
			std::thread m_worker;

			void wait_for_pool_construction(std::mutex& construction_lock) {
				std::lock_guard<std::mutex> lock(construction_lock);
			}

			void work(std::mutex& construction_lock) {
				wait_for_pool_construction(construction_lock);

				while (true) {

					if (m_stop.load(std::memory_order_acquire)) {
						return;
					}

					std::unique_ptr<callback_base> task;

					{
						std::lock_guard<spinlock> lock(m_lock);
						if (!m_tasks.empty()) {
							task = std::move(m_tasks.front());
							m_tasks.pop();
						}
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

					std::unique_lock<spinlock> lock(m_lock);
					m_condition.wait(lock, [&] {
						return !m_tasks.empty() || m_stop.load(std::memory_order_acquire);
					});

				}
			}

			std::unique_ptr<callback_base> try_steal() {
				std::unique_lock<spinlock> lock(m_lock, std::try_to_lock);

				if (lock.owns_lock() && !m_tasks.empty()) {
					auto task = std::move(m_tasks.front());
					m_tasks.pop();
					return task;
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
				auto task = make_callback(std::forward<function_type>(function));

				{
					std::lock_guard<spinlock> lock(m_lock);
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

			size_t choose_next_worker() noexcept {
				static thread_local size_t counter = 0;
				return ++counter % m_workers.size();
			}

			template<class function_type, class ... arguments>
			void enqueue_task(function_type&& function, arguments&& ... args) {
				enqueue_task(std::bind(
					std::forward<function_type>(function),
						std::forward<arguments>(args)...));
			}

			template<class function_type>
			void enqueue_task(function_type&& function) {
				auto self_worker = get_self_worker();

				if (self_worker != nullptr) {
					self_worker->enqueue_task(
						true,
						std::forward<function_type>(function));
					return;
				}

				const auto index = choose_next_worker();
				m_workers[index].enqueue_task(false,
					std::forward<function_type>(function));
			}

			static thread_pool& default_instance() {
				static thread_pool default_thread_pool(
					static_cast<size_t>(std::thread::hardware_concurrency() * 1.25));
				return default_thread_pool;
			}

		};

		enum class future_result_state {
			NOT_READY,
			RESULT,
			EXCEPTION
		};

		template<class T>
		union compressed_future_result {
			T result;
			std::exception_ptr exception;

			compressed_future_result() noexcept {};
			~compressed_future_result() noexcept {};

		};

		template<class T>
		union compressed_future_result<T&> {
			T* result;
			std::exception_ptr exception;

			compressed_future_result() noexcept {};
			~compressed_future_result() noexcept {};
		};


		class future_associated_state_base {

		protected:
			mutable spinlock m_lock;
			std::unique_ptr<callback_base> m_deffered, m_then;
			future_result_state m_state;
			mutable std::unique_ptr<std::condition_variable_any> m_condition;

			void build_condition_object() const {
				pool_allocator<std::condition_variable_any> allocator;
				auto cv = allocator.allocate(1);
				try {
					new (cv) std::condition_variable_any();
				}
				catch (...) {
					allocator.deallocate(cv, 1);
				}

				m_condition.reset(cv);
			}

		public:

			future_associated_state_base() noexcept :
				m_state(future_result_state::NOT_READY) {}

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
				std::unique_lock<spinlock> lock(m_lock);

				if (static_cast<bool>(m_deffered)) {
					return ::std::future_status::deferred;
				}

				if (m_state != future_result_state::NOT_READY) {			
					return ::std::future_status::ready;
				}

				const auto has_result = get_condition().wait_for(lock, duration, [this] {
					return m_state != future_result_state::NOT_READY;
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
				if (m_deffered) {
					m_deffered->execute();
					m_deffered.reset();
					assert(m_state != future_result_state::NOT_READY);
					return;
				}
				
				wait_for(std::chrono::hours(1'000 * 12 * 30 * 24));
			}

			template<class continuation_type>
			void set_continuation(continuation_type&& continuation) {
				std::lock_guard<spinlock> lock(m_lock);

				if (m_state != future_result_state::NOT_READY) {
					continuation();
					return;
				}

				assert(!static_cast<bool>(m_then));
				m_then = make_callback(std::forward<continuation_type>(continuation));
			}

			bool has_deffered_task() const noexcept {
				return static_cast<bool>(m_deffered);
			}

			bool is_ready() const noexcept {
				std::lock_guard<spinlock> lock(m_lock);
				return m_state != future_result_state::NOT_READY;
			}

			void call_continuation() {
				if (m_condition) {
					m_condition->notify_all();
				}

				if (static_cast<bool>(m_then)) {
					m_then->execute();
					m_then.reset();
				}
			}

		};

		template<class T>
		class future_associated_state :
			public future_associated_state_base {

		private:
			compressed_future_result<T> m_result;

			void destroy_result() noexcept {
				m_result.result.~T();
			}

			void destroy_exception() noexcept {
				m_result.exception.~exception_ptr();
			}

		public:

			future_associated_state() noexcept = default;

			~future_associated_state() noexcept {
				switch (m_state) {

					case future_result_state::RESULT: {
						destroy_result();
						return;
					}

					case future_result_state::EXCEPTION: {
						destroy_exception();
						return;
					}

				}
			}

			template<class ... arguments>
			void set_result(arguments&& ... args) {
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY);
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
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY);
				new (std::addressof(m_result.exception))
					std::exception_ptr(std::move(exception_pointer));
				m_state = future_result_state::EXCEPTION;
				call_continuation();
			}

			T result_or_exception() {
				std::lock_guard<spinlock> lock(m_lock);
				
				assert(m_state != future_result_state::NOT_READY);
				
				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_result.exception);
				}

				return std::move(m_result.result);
			}

			T get() {
				wait();
				return result_or_exception();
			}

			template<class function_type>
			void set_deffered_task(function_type&& function) {
				/*
					this function should only be called once,
					by using async + launch::deffered
				*/
				std::lock_guard<spinlock> lock(m_lock);
				
				assert(m_state == future_result_state::NOT_READY);
				assert(!static_cast<bool>(m_deffered));

				m_deffered = make_callback([this, _function = std::forward<function_type>(function)]{
					try {
						new (std::addressof(m_result.result)) T(_function());
						m_state = future_result_state::RESULT;
					}
					catch (...) {
						new (std::addressof(m_result.exception))
							std::exception_ptr(std::current_exception());
						m_state = future_result_state::EXCEPTION;
					}
				});
			}

		};

		template<>
		class future_associated_state<void> :
			public future_associated_state_base {

		private:
			std::exception_ptr m_exception;

		public:

			void set_exception(std::exception_ptr exception_pointer) {
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY);
				m_exception = std::move(exception_pointer);
				m_state = future_result_state::EXCEPTION;
				call_continuation();
			}

			void set_result() {
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY);
				m_state = future_result_state::RESULT;
				call_continuation();
			}

			void result_or_exception() {
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state != future_result_state::NOT_READY);
				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_exception);
				}
			}

			void get() {
				wait();
				result_or_exception();
			}

			template<class function_type>
			void set_deffered_task(function_type&& function) {
				/*
					this function should only be called once,
					by using async + launch::deffered
				*/
				std::lock_guard<spinlock> lock(m_lock);

				assert(m_state == future_result_state::NOT_READY);
				assert(!static_cast<bool>(m_deffered));

				m_deffered = make_callback([this, _function = std::forward<function_type>(function)]{
					try {
						_function();
						m_state = future_result_state::RESULT;
					}
					catch (...) {
						new (std::addressof(m_exception)) 
							std::exception_ptr(std::current_exception());
						m_state = future_result_state::EXCEPTION;
					}
				});
			}

		};

		template<class T>
		class future_associated_state<T&> :
			public future_associated_state_base {

			compressed_future_result<T&> m_result;

			void destroy_exception() noexcept {
				m_result.exception.~exception_ptr();
			}

		public:

			future_associated_state() noexcept = default;

			~future_associated_state() noexcept {
				if (m_state == future_result_state::EXCEPTION) {
					destroy_exception();
				}
			}

			void set_result(T& reference) {
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY);
				m_result.result = std::addressof(reference);
				m_state = future_result_state::RESULT;
				call_continuation();
			}

			template<class exception>
			void set_exception(exception&& given_exception) {
				set_exception(std::make_exception_ptr(std::forward<exception>(given_exception)));
			}

			void set_exception(std::exception_ptr exception_pointer) {
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state == future_result_state::NOT_READY);
				new (std::addressof(m_result.exception))
					std::exception_ptr(std::move(exception_pointer));
				m_state = future_result_state::EXCEPTION;
				call_continuation();
			}

			T& result_or_exception() {
				std::lock_guard<spinlock> lock(m_lock);
				assert(m_state != future_result_state::RESULT);
				
				if (m_state == future_result_state::EXCEPTION) {
					std::rethrow_exception(m_result.exception);
				}

				assert(m_result.result != nullptr);
				return *m_result.result;
			}

			T& get() {
				wait();
				return result_or_exception();
			}

			template<class function_type>
			void set_deffered_task(function_type&& function) {
				/*
					this function should only be called once,
					by using async + launch::deffered
				*/
				std::lock_guard<spinlock> lock(m_lock);

				assert(m_state == future_result_state::NOT_READY);
				assert(!static_cast<bool>(m_deffered));

				m_deffered = make_callback([this, _function = std::forward<function_type>(function)]{
					try {
						m_result.result = std::addressof(_function());
						m_state = future_result_state::RESULT;
					}
					catch (...) {
						new (std::addressof(m_exception)) std::exception_ptr(std::current_exception());
						m_state = future_result_state::EXCEPTION;
					}
				});
			}

		};

		/*
			This class SFINAEs the continuation functions set with
			future::then. depending on the continuation, a future<T/T&/void>
			should be returned. this class handles wiring the future with 
			the continuation.
		*/
		template<class function, class inner_type, class outer_type>
		class future_continuation {

		private:
			function m_function;
			std::shared_ptr<future_associated_state<inner_type>> m_old_state;
			std::shared_ptr<future_associated_state<outer_type>> m_new_state;

			/*
				Is the continuation returns void? if so
				a diffrent treatment should be applied
			*/
			using is_void = typename std::conditional_t<
				std::is_same<outer_type, void>::value,
				std::true_type,
				std::false_type>;

			void continue_impl(std::false_type is_void) noexcept {
				try {
					m_new_state->set_result(m_function(future<inner_type>(m_old_state)));
				}
				catch (...) {
					m_new_state->set_exception(std::current_exception());
				}
			}

			void continue_impl(std::true_type is_void) noexcept {
				try {
					m_function(future<inner_type>(m_old_state));
					m_new_state->set_result();
				}
				catch (...) {
					m_new_state->set_exception(std::current_exception());
				}
			}


		public:

			future_continuation(
				function given_function,
				decltype(m_old_state) old_state,
				decltype(m_new_state) new_state) :
				m_function(std::forward<function>(given_function)),
				m_old_state(std::move(old_state)),
				m_new_state(std::move(new_state)) {}

			void operator() () noexcept {
				continue_impl(is_void());
			}

		};

		class promise_base {

		protected:
			bool m_future_retreived;
			bool m_fulfilled;

			promise_base() noexcept : m_fulfilled(false), m_future_retreived(false) {}
			promise_base(promise_base&& rhs) noexcept = default;
			promise_base& operator = (promise_base&& rhs) noexcept = default;
		
		};

		template<class T>
		auto& get_inner_state(T& state_holder) noexcept {
			return state_holder.m_state;
		}

		template<class T , class scheduler_type>
		struct async_impl {

			template<class F>
			inline static future<T> do_async(F&& task) {
				future<T> future;
				pool_allocator<future_associated_state<T>> allocator;
				auto& inner_future_state = get_inner_state(future); //avoid the memory/cpu overhead of promise.
				inner_future_state = 
					std::allocate_shared<future_associated_state<T>>(allocator);
				scheduler_type scheduler;

				scheduler([future_state = inner_future_state, task = std::forward<F>(task)]() mutable{
					try {
						future_state->set_result(task());
					}
					catch (...) {
						future_state->set_exception(std::current_exception());
					}
				});

				return future;
			}
		};

		template<class scheduler_type>
		struct async_impl<void, scheduler_type> {

			template<class F>
			inline static future<void> do_async(F&& task) {
				future<void> future;
				pool_allocator<future_associated_state<void>> allocator;
				auto& inner_future_state = get_inner_state(future); //avoid the memory/cpu overhead of promise.
				inner_future_state = 
					std::allocate_shared<future_associated_state<void>>(allocator);
				scheduler_type scheduler;
				
				scheduler([future_state = inner_future_state, task = std::forward<F>(task)]() mutable{
					try {
						task();
						future_state->set_result();
					}
					catch (...) {
						future_state->set_exception(std::current_exception());
					}
				});

				return future;
			}

		};

		template<class T, class scheduler_type>
		struct async_impl<future<T>, scheduler_type> {

			template<class F>
			static inline future<T> do_async(F&& task) {
				future<T> future;
				pool_allocator<future_associated_state<T>> allocator;
				auto& inner_future_state = get_inner_state(future); //avoid the memory/cpu overhead of promise.
				inner_future_state = 
					std::allocate_shared<future_associated_state<T>>(allocator);
				scheduler_type scheduler;

				scheduler([future_state = inner_future_state, task = std::forward<F>(task)]()->void{
					auto future = task();
					future.then([future_state = std::move(future_state)](auto done_future){
						try {
							future_state->set_result(done_future.get());
						}
						catch (...) {
							future_state->set_exception(std::current_exception());
						}
					});
				});

				return future;
			}
		};

		template<class scheduler_type>
		struct async_impl<future<void>, scheduler_type> {

			template<class F>
			static inline future<void> do_async(F&& task) {
				future<void> future;
				pool_allocator<future_associated_state<void>> allocator;
				auto& inner_future_state = get_inner_state(future); //avoid the memory/cpu overhead of promise.
				inner_future_state = 
					std::allocate_shared<future_associated_state<void>>(allocator);
				scheduler_type scheduler;

				schedule([future_state = std::move(inner_future_state), task = std::forward<F>(task)]()->void{
					auto future = task();
					future.then([future_state = std::move(future_state)](auto done_future){
						try {
							done_future.get();
							future_state->set_result();
						}
						catch (...) {
							future_state->set_exception(std::current_exception());
						}
					});
				});

				return future;
			}
		};

		template<class result_type, class function_type, class ... arguments>
		future<result_type> deffered_async_impl(function_type&& function, arguments&& ... args) {
			future<result_type> future;
			pool_allocator<future_associated_state<result_type>> allocator;
			auto& state = get_inner_state(future);
			state = 
				std::allocate_shared<future_associated_state<result_type>>(allocator);
			state->set_deffered_task(std::bind(std::forward<function_type>(function), std::forward<arguments>(args)...));
			return future;
		}

		struct thread_pool_scheduler {	
			template<class function_type>
			void operator() (function_type&& function) {
				::concurrencpp::spawn(std::forward<function_type>(function));
			}
		};

		struct thread_scheduler {
			template<class function_type>
			void operator() (function_type&& function) {
				::std::thread execution_thread(std::forward<function_type>(function));
				execution_thread.detach();
			}
		};

	}

	template<class T>
	class future {

		template<class T> friend class ::concurrencpp::details::future_associated_state;
		template<class T> friend class ::concurrencpp::promise;
		template<class T> friend auto& details::get_inner_state(T& state_holder) noexcept;

	private:
		std::shared_ptr<details::future_associated_state<T>> m_state;

		void throw_if_empty() const {
			if (!static_cast<bool>(m_state)) {
				throw std::future_error(std::future_errc::no_state);
			}
		}

	public:

		future(decltype(m_state) state) :
			m_state(std::move(state)) {}

		future() noexcept = default;
		future(future&& rhs) noexcept = default;
		future& operator = (future&& rhds) noexcept = default;

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

		T get() {
			throw_if_empty();
			return m_state->get();
		}

		template<class continuation_type>
		auto then(continuation_type&& continuation) {
			throw_if_empty();

			using return_type = typename std::result_of_t<continuation_type(future<T>)>;
			details::pool_allocator<details::future_associated_state<return_type>> allocator;
			auto new_associated_state =
				std::allocate_shared<details::future_associated_state<return_type>>(allocator);

			m_state->set_continuation(
				details::future_continuation<continuation_type, T, return_type>(
					std::forward<continuation_type>(continuation),
					m_state,
					new_associated_state));

			return future<return_type>(new_associated_state);
		}

	};

	template<class result_type>
	future<result_type> make_ready_future(result_type&& result) {
		future<result_type> ready_future;
		details::pool_allocator<details::future_associated_state<result_type>> allocator;
		auto& future_inner_state = details::get_inner_state(ready_future);
		future_inner_state = 
			std::allocate_shared<details::future_associated_state<result_type>>(allocator);
		future_inner_state->set_result(std::forward<result_type>(result));
		return ready_future;
	}

	template<class result_type>
	future<result_type> make_exceptional_future(std::exception_ptr exception_pointer) {
		future<result_type> ready_future;
		details::pool_allocator<details::future_associated_state<result_type>> allocator;
		auto& future_inner_state = details::get_inner_state(ready_future);
		future_inner_state =
			std::allocate_shared<details::future_associated_state<result_type>>(allocator);
		future_inner_state->set_exception(exception_pointer);
		return ready_future;
	}

	template<class result_type, class exception_type>
	future<result_type> make_exceptional_future(exception_type exception) {
		return make_exceptional_future(std::make_exception_ptr(std::move(exception)));
	}

	template<class T>
	class promise : public details::promise_base {

	private:
		std::shared_ptr<details::future_associated_state<T>> m_state;

		void validate_state() {
			if (!static_cast<bool>(m_state)) {
				throw std::future_error(std::future_errc::no_state);
			}

			if (m_fulfilled) {
				throw std::future_error(std::future_errc::promise_already_satisfied);
			}
		}

	public:

		promise() noexcept = default;
		promise(promise&& rhs) noexcept = default;
		promise& operator = (promise&& rhs) noexcept = default;

		~promise() noexcept {
			if (static_cast<bool>(m_state) && 
				!m_fulfilled && 
				!m_state->has_deffered_task()) {
				m_state->set_exception(std::future_error(std::future_errc::broken_promise));
			}
		}

		future<T> get_future() {
			if (m_future_retreived) {
				throw std::future_error(std::future_errc::future_already_retrieved);
			}

			if (!static_cast<bool>(m_state)) {
				details::pool_allocator<details::future_associated_state<T>> allocator;
				m_state = 
					std::allocate_shared<details::future_associated_state<T>>(allocator);
			}

			m_future_retreived = true;
			return future<T>(m_state);
		}

		template<class ... arguments>
		void set_value(arguments&& ... args) {
			validate_state();
			m_state->set_result(std::forward<arguments>(args)...);
			m_fulfilled = true;
		}

		void set_exception(std::exception_ptr exception_pointer) {
			validate_state();
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

		void validate_state() {
			if (!static_cast<bool>(m_state)) {
				throw std::future_error(std::future_errc::no_state);
			}

			if (m_fulfilled) {
				throw std::future_error(std::future_errc::promise_already_satisfied);
			}
		}

	public:
		promise() noexcept = default;
		promise(promise&& rhs) noexcept = default;
		promise& operator = (promise&& rhs) noexcept = default;

		~promise() noexcept {
			if (static_cast<bool>(m_state) && !m_fulfilled) {
				m_state->set_exception(
					std::make_exception_ptr(std::future_error(std::future_errc::broken_promise)));
			}
		}

		future<void> get_future() {
			if (m_future_retreived) {
				throw std::future_error(std::future_errc::future_already_retrieved);
			}

			if (!static_cast<bool>(m_state)) {
				details::pool_allocator<details::future_associated_state<void>> allocator;
				m_state =
					std::allocate_shared<details::future_associated_state<void>>(allocator);
			}

			m_future_retreived = true;
			return future<void>(m_state);
		}

		void set_value() {
			validate_state();
			m_state->set_result();
			m_fulfilled = true;
		}

		void set_exception(std::exception_ptr exception_pointer) {
			validate_state();
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
		details::thread_pool::default_instance().enqueue_task(std::forward<function>(func));
	}

	enum class launch {
		async,
		deferred,
		task
	};

	template <class F, class... Args>
	auto async(launch launch_policy, F&& f, Args&&... args){
		using function_type = typename std::decay_t<F>;
		using result = typename std::result_of_t<function_type(Args...)>;

		switch (launch_policy) {

			case launch::task: {
				return details::async_impl<result, details::thread_pool_scheduler>::do_async(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
			}

			case launch::deferred: {
				return details::deffered_async_impl<result>(std::forward<F>(f), std::forward<Args>(args)...);
			}

			case launch::async: {
				return details::async_impl<result, details::thread_scheduler>::do_async(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
			}

		}

		assert(false);
		return future<result>();
	}

	template <class F, class... Args>
	auto async(launch launch_policy, F&& f) {
		using function_type = typename std::decay_t<F>;
		using result = typename std::result_of_t<function_type()>;

		switch (launch_policy) {

			case launch::task: {
				return details::async_impl<result, details::thread_pool_scheduler>::do_async(std::forward<F>(f));
			}

			case launch::deferred: {
				return details::deffered_async_impl<result>(std::forward<F>(f));
			}

			case launch::async: {
				return details::async_impl<result, details::thread_scheduler>::do_async(std::forward<F>(f));
			}

		}

		assert(false);
		return future<result>();
	}

}
