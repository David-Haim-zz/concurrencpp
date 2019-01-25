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

#include "concurrencpp.h"

#include <array>
#include <climits>

namespace concurrencpp {
	namespace details {
		class virtual_affinity_manager {

		private:
			std::atomic_size_t m_next_token;
			const size_t m_logical_processors;

			size_t next_token() noexcept;

			static size_t number_of_processors() noexcept;

			static virtual_affinity_manager s_virtual_affinity_manager_instance;

		public:
			virtual_affinity_manager() noexcept;

			inline size_t get_number_of_logical_processors() const noexcept { return m_logical_processors; }
			static size_t get_this_thread_affinity() noexcept;
			static virtual_affinity_manager& default_instance() noexcept;
		};
	}
}

/*
	virtual_affinity_manager implementation
*/

concurrencpp::details::virtual_affinity_manager 
concurrencpp::details::virtual_affinity_manager::s_virtual_affinity_manager_instance;

size_t concurrencpp::details::virtual_affinity_manager::number_of_processors() noexcept {
	const auto hardware_concurrency = std::thread::hardware_concurrency();
	return hardware_concurrency == 0 ? 8 : hardware_concurrency;
}

size_t concurrencpp::details::virtual_affinity_manager::next_token() noexcept {
	return m_next_token.fetch_add(1, std::memory_order_acq_rel) % m_logical_processors;
}

concurrencpp::details::virtual_affinity_manager::virtual_affinity_manager() noexcept :
	m_logical_processors(number_of_processors()),
	m_next_token(0) {}

size_t concurrencpp::details::virtual_affinity_manager::get_this_thread_affinity() noexcept {
	static thread_local const size_t s_affinity = s_virtual_affinity_manager_instance.next_token();
	return s_affinity;
}

concurrencpp::details::virtual_affinity_manager &
concurrencpp::details::virtual_affinity_manager::default_instance() noexcept {
	return s_virtual_affinity_manager_instance;
}

namespace concurrencpp {
	namespace details {

		class memory_block;
		class arena;
		class memory_pool;

		void* allocate_aligned(size_t alignment, size_t size);
		void free_aligned(void* ptr);

		constexpr size_t k_page_size = 4 * 1024;
		constexpr size_t k_buffer_size = 2 * k_page_size;

		struct alignas(std::max_align_t) header {
			memory_block* const block;

			inline header(memory_block* block) noexcept : block(block) {}
		};

		struct chunk {
			chunk* next;
		};

		class arena {

		private:
			spinlock m_lock;
			memory_block *m_available_blocks, *m_full_blocks;
			size_t m_number_of_blocks;
			size_t m_num_of_available_blocks, m_num_of_full_blocks;
			const size_t m_chunk_size, m_max_objects_per_block;

			void assert_state() const noexcept;
			bool allocate_new_block() noexcept;
			void* allocate_from_available_block() noexcept;

		public:
			inline arena(size_t chunk_size) noexcept : 
				m_chunk_size(chunk_size),
				m_max_objects_per_block(k_buffer_size / chunk_size),
				m_available_blocks(nullptr),
				m_num_of_available_blocks(0),
				m_num_of_full_blocks(0),
				m_full_blocks(nullptr),
				m_number_of_blocks(0) {}

			~arena() noexcept;

			void handle_block_available(memory_block& block) noexcept;
			void handle_block_full(memory_block& block) noexcept;
			void handle_block_free(memory_block& block) noexcept;

			void* allocate() noexcept;

			inline size_t chunk_size() const noexcept { return m_chunk_size; }
			inline size_t max_objects_per_block() const noexcept { return m_max_objects_per_block; }
			inline std::unique_lock<decltype(m_lock)> lock_arena() noexcept { return std::unique_lock<decltype(m_lock)>{ m_lock }; }
		};

		class memory_block {

		private:
			memory_block *m_before, *m_after;
			size_t m_num_of_free_chunks;
			char *m_cursor;
			std::unique_ptr<char, void(*)(void*)> m_buffer;
			chunk *m_first_chunk;
			arena& m_parent_arena;

			inline memory_block(arena& parent_arena, memory_block* next, char* buffer) noexcept :
				m_before(nullptr),
				m_after(next),
				m_first_chunk(nullptr),
				m_num_of_free_chunks(0),
				m_parent_arena(parent_arena),
				m_buffer(buffer, &concurrencpp::details::free_aligned),
				m_cursor(buffer) {}

		public:
			inline const char* end() const noexcept { return m_buffer.get() + k_buffer_size; }
			inline size_t chunk_size() const noexcept { return m_parent_arena.chunk_size(); }
			inline size_t max_objects_per_block() const noexcept { return m_parent_arena.max_objects_per_block(); }
			inline bool is_full() const noexcept { return m_num_of_free_chunks == 0 && m_cursor == end(); }
			inline memory_block* get_before() noexcept { return m_before; }
			inline memory_block* get_after() noexcept { return m_after; }
			inline void set_before(memory_block* before) noexcept { m_before = before; }
			inline void set_after(memory_block* after) noexcept { m_after = after; }
			inline size_t num_of_free_chunks() const noexcept { return m_num_of_free_chunks; }
			inline bool has_free_buffer() const noexcept { return m_cursor < end(); }
			void detach() noexcept;
			void make_new_head(memory_block*& head) noexcept;
			char* allocate() noexcept;
			void deallocate(void* memory) noexcept;

			static memory_block* new_block(arena& parent_arena, memory_block* next) noexcept;
		};
	}
}

/*
	memory_block implementation
*/

void concurrencpp::details::memory_block::detach() noexcept {
	if (m_before != nullptr) {
		m_before->m_after = m_after;
	}

	if (m_after != nullptr) {
		m_after->m_before = m_before;
	}

	m_after = nullptr;
	m_before = nullptr;
}

void concurrencpp::details::memory_block::make_new_head(memory_block* & head) noexcept {
	assert((head != nullptr) ? head->m_before == nullptr : true);

	m_before = nullptr;
	m_after = head;

	if (head != nullptr) {
		head->m_before = this;
	}

	head = this;
}

char * concurrencpp::details::memory_block::allocate() noexcept {
	assert(!is_full());

	char* result = nullptr;

	if (m_first_chunk != nullptr) {
		result = reinterpret_cast<char*>(m_first_chunk);
		m_first_chunk = m_first_chunk->next;
		--m_num_of_free_chunks;
	}
	else if (m_cursor < end()) {
		result = m_cursor;
		m_cursor += chunk_size();
	}
	else {
		//We shouldn't reach here. this branch means the block is full, but isn't removed from
		//the available free blocks-chain
		assert(false);
	}

	if (is_full()) {
		m_parent_arena.handle_block_full(*this);
	}

	auto _header = new (result) header(this);

	auto memory = reinterpret_cast<char*>(_header) + sizeof(header);
	assert(reinterpret_cast<size_t>(memory) % alignof(std::max_align_t) == 0);

	return memory;
}

void concurrencpp::details::memory_block::deallocate(void * memory) noexcept {
	assert(static_cast<char*>(memory) >= m_buffer.get());
	assert(static_cast<char*>(memory) + chunk_size() <= end());
	assert(reinterpret_cast<size_t>(memory) % alignof(chunk) == 0);

#if defined(_DEBUG)
	::memset(memory, 0x6, chunk_size());
#endif

	chunk* _chunk = new (memory) chunk();

	auto lock = m_parent_arena.lock_arena();

	_chunk->next = m_first_chunk;
	m_first_chunk = _chunk;
	++m_num_of_free_chunks;
	assert(m_num_of_free_chunks <= max_objects_per_block());

	if (m_num_of_free_chunks == max_objects_per_block()) { //all chunks deallocated to the pool
		assert(m_cursor == end());
		return m_parent_arena.handle_block_free(*this);
	}
	else if (m_num_of_free_chunks == 1 && m_cursor == end()) { //the block has been full until now
		return m_parent_arena.handle_block_available(*this);
	}
}

concurrencpp::details::memory_block * 
concurrencpp::details::memory_block::new_block(arena & parent_arena, memory_block * next) noexcept {
	std::unique_ptr<void, decltype(&free_aligned)> buffer = { allocate_aligned(k_page_size, k_buffer_size), &free_aligned };

	if (!static_cast<bool>(buffer)) {
		return nullptr;
	}

	auto block = new (std::nothrow) memory_block(parent_arena, next, static_cast<char*>(buffer.get()));
	if (block == nullptr) {
		return nullptr;
	}

	buffer.release();
	return block;
}

/*
	arena implementation
*/

concurrencpp::details::arena::~arena() noexcept {
	auto cursor = m_available_blocks;
	while (cursor != nullptr) {
		auto next = cursor->get_after();
		delete cursor;
		cursor = next;
	}
}

void concurrencpp::details::arena::assert_state() const noexcept {
	if (m_num_of_available_blocks > 0) {
		assert(m_available_blocks != nullptr);
		assert(m_available_blocks->num_of_free_chunks() > 0 || m_available_blocks->has_free_buffer());
	}

	if (m_num_of_full_blocks > 0) {
		assert(m_full_blocks != nullptr);
		assert(m_full_blocks->num_of_free_chunks() == 0 && !m_full_blocks->has_free_buffer());
	}
}

bool concurrencpp::details::arena::allocate_new_block() noexcept {
	auto block = memory_block::new_block(*this, m_available_blocks);
	if (block == nullptr) {
		return false;
	}

	m_available_blocks = block;
	auto after = m_available_blocks->get_after();
	if (after != nullptr) {
		after->set_before(m_available_blocks);
	}

	++m_number_of_blocks;
	++m_num_of_available_blocks;
	return true;
}

void concurrencpp::details::arena::handle_block_available(memory_block & block) noexcept {
	assert(m_full_blocks != nullptr);
	assert(m_num_of_full_blocks > 0);
	assert(block.num_of_free_chunks() == 1);

	if (&block == m_full_blocks) {
		m_full_blocks = m_full_blocks->get_after();
	}

	block.detach();
	block.make_new_head(m_available_blocks);
	
	++m_num_of_available_blocks;
	--m_num_of_full_blocks;
}

void concurrencpp::details::arena::handle_block_full(memory_block & block) noexcept {
	assert(block.is_full());
	assert(m_num_of_available_blocks > 0);
	assert(m_available_blocks != nullptr);

	//take the block and push it to the full_blocks chain
	if (&block == m_available_blocks) {
		m_available_blocks = m_available_blocks->get_after();
	}

	block.detach();
	block.make_new_head(m_full_blocks);

	--m_num_of_available_blocks;
	++m_num_of_full_blocks;
}

void concurrencpp::details::arena::handle_block_free(memory_block & block) noexcept {
	assert(m_num_of_available_blocks > 0);
	assert(block.num_of_free_chunks() == max_objects_per_block());
	assert(!block.has_free_buffer());
	
	--m_number_of_blocks;
	--m_num_of_available_blocks;

	if (&block == m_available_blocks) {
		m_available_blocks = m_available_blocks->get_after();
	}

	block.detach();
	delete &block;
}

void * concurrencpp::details::arena::allocate_from_available_block() noexcept {
	assert(m_num_of_available_blocks > 0);
	assert(m_available_blocks != nullptr);
	assert(m_available_blocks->has_free_buffer() || m_available_blocks->num_of_free_chunks() > 0);

	auto memory = m_available_blocks->allocate();
	assert(memory != nullptr);
	return memory;
}

void * concurrencpp::details::arena::allocate() noexcept {
	auto lock = lock_arena();
	assert_state();

	if (m_available_blocks != nullptr || allocate_new_block()) {
		return allocate_from_available_block();
	}

	return nullptr;
}

namespace concurrencpp {
	namespace details {
		class memory_pool {

		private:
			arena m_arenas[6] = { 32, 64, 128, 256, 512, 1024 };
			static const std::array<uint8_t, 1025> s_pool_index_table;

			static std::array<uint8_t, 1025> build_jump_table() noexcept;
			static size_t get_pool_index(size_t size) noexcept;

		public:
			void* allocate(size_t size) noexcept;
		};
	}
}

/*
	memory_pool implementation
*/

const std::array<uint8_t, 1025> concurrencpp::details::memory_pool::s_pool_index_table = memory_pool::build_jump_table();

std::array<uint8_t, 1025> concurrencpp::details::memory_pool::build_jump_table() noexcept {
	std::array<uint8_t, 1025> token_table;
	for (size_t i = 0; i <= 1024; i++) token_table[i] = 5;
	for (size_t i = 0; i <= 512; i++) token_table[i] = 4;
	for (size_t i = 0; i <= 256; i++) token_table[i] = 3;
	for (size_t i = 0; i <= 128; i++) token_table[i] = 2;
	for (size_t i = 0; i <= 64; i++) token_table[i] = 1;
	for (size_t i = 0; i <= 32; i++) token_table[i] = 0;
	return token_table;
}

size_t concurrencpp::details::memory_pool::get_pool_index(size_t size) noexcept {
	assert(size != 0);
	assert(size <= 1024);
	assert(std::size(s_pool_index_table) == 1025);
	return s_pool_index_table[size];
}

void* concurrencpp::details::memory_pool::allocate(size_t size) noexcept {
	const auto token = get_pool_index(size);
	assert(token < std::size(m_arenas));
	return m_arenas[token].allocate();
}

/*
	memory_manager implementation
*/

namespace concurrencpp {
	namespace details {
		class memory_manager {

		private:
			std::unique_ptr<memory_pool[]> m_pools;
			size_t m_number_of_pools;

			static memory_manager s_memory_manager;
			static constexpr size_t k_biggest_chunk = 1'024;

			static header* header_from_ptr(void* ptr) noexcept;

			inline static size_t adjust_allocation_size(size_t size) noexcept { return size + sizeof(header); }

		public:
			memory_manager();

			inline static memory_manager& default_instance() noexcept { return s_memory_manager; }

			void* allocate(size_t size) noexcept;
			static void deallocate(void* pointer, size_t size) noexcept;
		};
	}
}

concurrencpp::details::memory_manager concurrencpp::details::memory_manager::s_memory_manager;

concurrencpp::details::header * concurrencpp::details::memory_manager::header_from_ptr(void * ptr) noexcept {
	auto header_ptr = static_cast<header*>(ptr);
	--header_ptr;
	assert(reinterpret_cast<size_t>(header_ptr) % alignof(header) == 0);
	return header_ptr;
}

concurrencpp::details::memory_manager::memory_manager() {
	m_number_of_pools = virtual_affinity_manager::default_instance().get_number_of_logical_processors();
	m_pools = std::make_unique<memory_pool[]>(m_number_of_pools);
}

void * concurrencpp::details::memory_manager::allocate(size_t size) noexcept {
	if (size == 0) {
		return nullptr;
	}

	//if ((x > 0) && (a > INT_MAX - x)) /* `a + x` would overflow */;
	if (sizeof(header) > UINTMAX_MAX - size) {
		return nullptr;
	}

	size = adjust_allocation_size(size);

	if (size > k_biggest_chunk) {
		return ::std::malloc(size);
	}

	const auto pool_number = virtual_affinity_manager::default_instance().get_this_thread_affinity();
	assert(pool_number < m_number_of_pools);

	auto result = m_pools[pool_number].allocate(size);
	if (static_cast<bool>(result)) {
		return result;
	}

	for (size_t i = 0; i < m_number_of_pools; i++) {
		result = m_pools[i].allocate(size);
		if (static_cast<bool>(result)) {
			return result;
		}
	}

	return nullptr;
}

void concurrencpp::details::memory_manager::deallocate(void * pointer, size_t size) noexcept {
	assert(size != 0);
	assert(pointer != nullptr);

	size = adjust_allocation_size(size);

	if (size > k_biggest_chunk) {
		return ::std::free(pointer);
	}

	auto header = header_from_ptr(pointer);
	auto block = header->block;

	assert(size <= block->chunk_size());

	block->deallocate(reinterpret_cast<char*>(header));
}

void * concurrencpp::details::allocate(size_t size) noexcept {
	return memory_manager::default_instance().allocate(size);
}

void concurrencpp::details::deallocate(void * pointer, size_t size) noexcept {
	if (pointer != nullptr) {
		memory_manager::default_instance().deallocate(pointer, size);
	}
}

/*
	spinlock implementation
*/

bool concurrencpp::details::spinlock::try_lock_once(void (&on_fail_callable)()) noexcept {
	size_t counter = 0ul;

	//make the processor yield
	while (locked.test_and_set(std::memory_order_acquire)) {
		if (counter == 128) {
			return false;
		}

		++counter;
		on_fail_callable();
	}

	return true;
}

void concurrencpp::details::spinlock::lock() noexcept {
	if (try_lock_once(pause_cpu)) {
		return;
	}

	if (try_lock_once(std::this_thread::yield)) {
		return;
	}

	//std::this_thread::sleep_for is noexecpt if used with standard clocks.
	while (try_lock_once(sleep) == false);
}

/*
	recursive_spinlock implementation
*/

void concurrencpp::details::recursive_spinlock::lock() noexcept {
	const auto this_thread = std::this_thread::get_id();
	std::thread::id no_id;
	if (m_owner.load(std::memory_order_acquire) == this_thread) {
		++m_count;
		return;
	}

	m_lock.lock();
	m_owner.store(this_thread, std::memory_order_release);
	m_count = 1;
}

void concurrencpp::details::recursive_spinlock::unlock() noexcept {
	assert(m_owner == std::this_thread::get_id());
	assert(m_count > 0);

	--m_count;
	if (m_count != 0) {
		return;
	}

	m_owner.store(std::thread::id(), std::memory_order_release);
	m_lock.unlock();
}

/*
	work_queue implementation
*/

namespace concurrencpp {
	namespace details {
		class work_queue {
		private:
			std::unique_ptr<callback_base> m_head;
			callback_base* m_tail;

		public:
			inline work_queue() noexcept : m_tail(nullptr) {}
			void push(std::unique_ptr<callback_base> task) noexcept;
			std::unique_ptr<callback_base> try_pop() noexcept;
			inline bool empty() const noexcept { return !static_cast<bool>(m_head); }
		};
	}
}

void concurrencpp::details::work_queue::push(std::unique_ptr<callback_base> task) noexcept {
	if (m_head == nullptr) {
		assert(m_tail == nullptr);
		m_tail = task.get();
		m_head = std::move(task);
	}
	else {
		assert(m_tail != nullptr);
		assert(m_tail->next.get() == nullptr);
		m_tail->next = std::move(task);
		m_tail = m_tail->next.get();
	}
}

std::unique_ptr<concurrencpp::details::callback_base> concurrencpp::details::work_queue::try_pop() noexcept {
	if (!static_cast<bool>(m_head)) {
		assert(m_tail == nullptr);
		return {};
	}

	assert(m_tail != nullptr);
	auto& next = m_head->next;
	auto task = std::move(m_head);
	m_head = std::move(next);

	if (m_head == nullptr) {
		m_tail = nullptr;
	}

	return task;
}

/*
	worker_thread implementation
*/

namespace concurrencpp {
	namespace details {
		class worker_thread {

		private:
			spinlock m_lock;
			work_queue m_tasks;
			thread_pool& m_parent_thread_pool;
			std::condition_variable_any m_condition;
			std::thread m_worker;

			void work();

		public:
			worker_thread(thread_pool& parent_thread_pool);
			worker_thread(worker_thread&& rhs) noexcept;

			std::unique_ptr<callback_base> try_steal() noexcept;

			template<class function_type>
			void enqueue_task(bool self, function_type&& function) {
				enqueue_task(make_callback(std::forward<function_type>(function)));
			}

			void enqueue_task(bool self, std::unique_ptr<callback_base> task) noexcept;

			inline void notify() noexcept { m_condition.notify_one(); }
			inline void join() { m_worker.join(); }
			inline std::thread::id get_id() const noexcept { return m_worker.get_id(); }
		};

	}
}

concurrencpp::details::worker_thread::worker_thread(thread_pool& parent_thread_pool) :
	m_parent_thread_pool(parent_thread_pool){
	m_worker = std::thread([this] { work(); });
}

concurrencpp::details::worker_thread::worker_thread(worker_thread&& rhs) noexcept :
	m_parent_thread_pool(rhs.m_parent_thread_pool) {
	assert(false); // this function shouldn't be called anyway.
}

void concurrencpp::details::worker_thread::work() {
	m_parent_thread_pool.wait_for_pool_construction();

	while (m_parent_thread_pool.is_working()) {
		std::unique_ptr<callback_base> task;

		{
			std::lock_guard<decltype(m_lock)> lock(m_lock);
			task = m_tasks.try_pop();
		}

		if (task) {
			task->execute();
			continue;
		}

		task = m_parent_thread_pool.dequeue_task();
		if (task) {
			task->execute();
			continue;
		}

		std::unique_lock<decltype(m_lock)> lock(m_lock);
		m_condition.wait(lock, [& , this] {
			return !m_tasks.empty() || !m_parent_thread_pool.is_working();
		});
	}
}

std::unique_ptr<concurrencpp::details::callback_base> 
concurrencpp::details::worker_thread::try_steal() noexcept {
	std::unique_lock<decltype(m_lock)> lock(m_lock, std::try_to_lock);

	if (lock.owns_lock() && !m_tasks.empty()) {
		return m_tasks.try_pop();
	}

	return{};
}

void concurrencpp::details::worker_thread::enqueue_task(bool self, std::unique_ptr<callback_base> task) noexcept {
	{
		std::unique_lock<decltype(m_lock)> lock(m_lock);
		m_tasks.push(std::move(task));
	}

	if (!self) {
		m_condition.notify_one();
	}
}

/*
	thread_pool implementation
*/

size_t concurrencpp::details::thread_pool::number_of_threads_cpu() noexcept {
	const auto concurrency_level = std::thread::hardware_concurrency();
	return concurrency_level == 0 ? 8 : static_cast<size_t>(concurrency_level * 1.25f);
}

size_t concurrencpp::details::thread_pool::number_of_threads_io() noexcept {
	const auto concurrency_level = std::thread::hardware_concurrency();
	return concurrency_level == 0 ? 8 : (concurrency_level * 2);
}

size_t concurrencpp::details::thread_pool::choose_next_worker() noexcept {
	static thread_local size_t counter = 0;
	return (++counter) % m_workers.size();
}

concurrencpp::details::worker_thread * concurrencpp::details::thread_pool::get_self_worker_impl() noexcept {
	const auto this_thread_id = std::this_thread::get_id();
	for (auto& worker : m_workers) {
		if (worker.get_id() == this_thread_id) {
			return &worker;
		}
	}

	return nullptr;
}

concurrencpp::details::worker_thread * concurrencpp::details::thread_pool::get_self_worker() noexcept {
	static thread_local auto cached_worker_thread = get_self_worker_impl();
	return cached_worker_thread;
}

concurrencpp::details::thread_pool::thread_pool(const size_t number_of_workers) :
	m_stop(false) {
	std::lock_guard<std::mutex> construction_lock(m_pool_lock);
	m_workers.reserve(number_of_workers);
	for (auto i = 0ul; i < number_of_workers; i++) {
		m_workers.emplace_back(*this);
	}
}

concurrencpp::details::thread_pool::~thread_pool() noexcept {
	m_stop.store(true, std::memory_order_release);

	for (auto& worker : m_workers) {
		worker.notify();
	}

	std::this_thread::yield();

	for (auto& worker : m_workers) {
		worker.join();
	}
}

void concurrencpp::details::thread_pool::wait_for_pool_construction(){
	std::unique_lock<decltype(m_pool_lock)> lock(m_pool_lock);
}

bool concurrencpp::details::thread_pool::is_working() noexcept{
	return !m_stop.load(std::memory_order_acquire);
}

std::unique_ptr<concurrencpp::details::callback_base>
concurrencpp::details::thread_pool::dequeue_task() noexcept{
	for (auto& worker : m_workers) {
		auto task = worker.try_steal();
		if (task) {
			return task;
		}
	}

	return {};
}

void concurrencpp::details::thread_pool::enqueue_task(std::unique_ptr<callback_base> function) noexcept {
	auto self_worker = get_self_worker();

	if (self_worker != nullptr ) {
		self_worker->enqueue_task(true, std::move(function));
		return;
	}

	const auto index = choose_next_worker();
	m_workers[index].enqueue_task(false, std::move(function));
}

concurrencpp::details::thread_pool & concurrencpp::details::thread_pool::default_instance() {
	static thread_pool s_default_thread_pool(number_of_threads_cpu());
	return s_default_thread_pool;
}

concurrencpp::details::thread_pool & concurrencpp::details::thread_pool::blocking_tasks_instance() {
	static thread_pool s_blocking_tasks_thread_pool(number_of_threads_io());
	return s_blocking_tasks_thread_pool;
}

/*
	continuation_scheduler implementation
*/
void concurrencpp::details::continuation_scheduler::schedule_coro_threadpool(std::experimental::coroutine_handle<void> handle){
	thread_pool::default_instance().enqueue_task(handle);
}

void concurrencpp::details::continuation_scheduler::schedule_cb_threadpool(std::unique_ptr<callback_base> cb){
	thread_pool::default_instance().enqueue_task(std::move(cb));
}

concurrencpp::details::continuation_scheduler 
concurrencpp::details::continuation_scheduler::thread_pool_scheduler(schedule_coro_threadpool, schedule_cb_threadpool);

/*
	future_associated_state_base implementation
*/

void concurrencpp::details::future_associated_state_base::execute_continuation_inline(){
	if (static_cast<bool>(m_coro_handle)) {
		return m_coro_handle();
	}

	if (static_cast<bool>(m_then)) {
		auto then = std::move(m_then); //make sure then is released either way.
		return then->execute();
	}
}

void concurrencpp::details::future_associated_state_base::schedule_continuation(){
	assert(m_scheduler != nullptr);
	if (static_cast<bool>(m_coro_handle)) {
		return m_scheduler->schedule_coro(m_coro_handle);
	}

	if (static_cast<bool>(m_then)) {
		return m_scheduler->schedule_cb(std::move(m_then));
	}
}

void concurrencpp::details::future_associated_state_base::build_condition_object() const {
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

std::condition_variable_any & concurrencpp::details::future_associated_state_base::get_condition_object() {
	if (!static_cast<bool>(m_condition)) {
		build_condition_object();
	}

	assert(static_cast<bool>(m_condition));
	return *m_condition;
}

const std::condition_variable_any & concurrencpp::details::future_associated_state_base::get_condition_object() const {
	if (!static_cast<bool>(m_condition)) {
		build_condition_object();
	}

	assert(static_cast<bool>(m_condition));
	return *m_condition;
}

void concurrencpp::details::future_associated_state_base::wait() {
	/*
		According to the standard, only non-timed wait on the future
		will cause the deffered-function to be launched. this is why
		this segment is not in wait_for implementation.
	*/

	{
		std::unique_lock<decltype(m_lock)> lock(m_lock);
		if (m_status == future_result_status::DEFFERED) {
			assert(static_cast<bool>(m_deffered));
			m_deffered->execute();

			assert(m_status != future_result_status::DEFFERED);
			assert(m_status != future_result_status::NOT_READY);
			return;
		}
	}

	const auto forever = std::chrono::milliseconds(std::numeric_limits<int>::max());
	while (wait_for(forever) == std::future_status::timeout);
}

void concurrencpp::details::future_associated_state_base::set_deffered_task(std::unique_ptr<callback_base> task) noexcept {
	// this function should only be called once, by using async + launch::deffered
	std::unique_lock<decltype(m_lock)> lock(m_lock);

	assert(m_status == future_result_status::NOT_READY);
	assert(!static_cast<bool>(m_deffered));
	assert(!static_cast<bool>(m_coro_handle));
	assert(!static_cast<bool>(m_then));

	m_deffered = std::move(task);
	m_status = future_result_status::DEFFERED;
}

void concurrencpp::details::future_associated_state_base::set_coro_handle(std::experimental::coroutine_handle<void> coro_handle) noexcept {
	auto state = future_result_status::NOT_READY;

	{
		std::unique_lock<decltype(m_lock)> lock(m_lock);

		assert(!static_cast<bool>(m_coro_handle));
		assert(!static_cast<bool>(m_then));

		state = m_status;

		if (state == future_result_status::NOT_READY) {
			m_coro_handle = coro_handle;
			return;
		}
	}

	if (state == future_result_status::DEFFERED) {
		assert(static_cast<bool>(m_deffered));
		m_deffered->execute();
		m_deffered.reset();

		assert(m_status != future_result_status::DEFFERED);
		assert(m_status != future_result_status::NOT_READY);
	}

	//state is either result or exception
	coro_handle();
}

void concurrencpp::details::future_associated_state_base::set_exception(std::exception_ptr exception_pointer, std::exception_ptr * exception_storage) {
	std::unique_lock<decltype(m_lock)> lock(m_lock);
	assert(m_status == future_result_status::NOT_READY || m_status == future_result_status::DEFFERED);
	new (exception_storage) std::exception_ptr(std::move(exception_pointer));
	m_status = future_result_status::EXCEPTION;
	call_continuation();
}

void concurrencpp::details::future_associated_state_base::call_continuation() {
	if (m_scheduler == nullptr) { //the usual case
		 execute_continuation_inline();
	}
	else {
		schedule_continuation();
	}

	if (static_cast<bool>(m_condition)) {
		m_condition->notify_all(); //do not reset the CV, as other objects wait on it on another thread.
	}
}

/*
	timer_queue interface. needs to be declared before the implementation of timer_impl
*/
namespace concurrencpp {
	namespace details {
		class timer_queue {

			using timer_ptr = std::shared_ptr<timer_impl>;
			constexpr static auto s_sleep_forever = std::numeric_limits<int>::max();

		private:
			std::recursive_mutex m_lock;
			std::condition_variable_any m_condition_variable;
			timer_ptr m_timers;
			size_t m_last_time_point, m_minimum_waiting_time;
			thread_pool& m_thread_pool;
			std::atomic_bool m_cancel;
			std::thread m_scheduler_thread;

			static timer_queue s_timer_queue_instance;

			void process_timers();
			void work_loop();

		public:
			inline timer_queue() :
				m_cancel(false),
				m_thread_pool(thread_pool::default_instance()),
				m_last_time_point(time_from_epoch()),
				m_minimum_waiting_time(s_sleep_forever) {
				m_scheduler_thread = std::thread([this] {work_loop(); });
			}

			~timer_queue() noexcept;

			inline static timer_queue& default_instance() noexcept { return s_timer_queue_instance; }

			void add_timer(timer_ptr timer) noexcept;
			void remove_timer(timer_impl& timer) noexcept;
		};
	}
}

/*
	Timer implementation
*/

concurrencpp::details::timer_impl::status 
concurrencpp::details::timer_impl::update_timer_due_time(const size_t diff) noexcept {
	assert(!m_has_due_time_reached);
	
	if (m_next_fire_time <= diff) {
		if (m_is_oneshot) {
			return status::SCHEDULE_DELETE;
		}

		//repeating timer:
		m_has_due_time_reached = true;
		m_next_fire_time = m_frequency.load(std::memory_order_acquire);
		return status::SCHEDULE;
	}

	//due time has not passed
	m_next_fire_time -= diff;
	return status::IDLE;
}

concurrencpp::details::timer_impl::status
concurrencpp::details::timer_impl::update_timer_frequency(const size_t diff) noexcept {
	assert(m_has_due_time_reached);
	
	if (m_next_fire_time <= diff) {
		m_next_fire_time = m_frequency.load(std::memory_order_acquire);
		return status::SCHEDULE;
	}

	m_next_fire_time -= diff;
	return status::IDLE;
}

concurrencpp::details::timer_impl::status
concurrencpp::details::timer_impl::update() noexcept {	
	status current_status;
	const auto now = time_from_epoch();
	assert(now >= m_last_update_time);
	const auto diff = now - m_last_update_time;
	if (m_has_due_time_reached) {
		current_status = update_timer_frequency(diff);
	}
	else {
		current_status = update_timer_due_time(diff);
	}

	m_last_update_time = now;
	return current_status;
}

void concurrencpp::details::timer_impl::set_initial_update_time() noexcept{
	m_last_update_time = time_from_epoch();
}

void concurrencpp::details::timer_impl::cancel() noexcept{
	auto& timer_queue = timer_queue::default_instance();
	timer_queue.remove_timer(*this);
}

/*
	timer_queue implementation
*/
concurrencpp::details::timer_queue concurrencpp::details::timer_queue::s_timer_queue_instance;

concurrencpp::details::timer_queue::~timer_queue() noexcept{
	m_cancel.store(true, std::memory_order_release);
	m_condition_variable.notify_all();
	m_scheduler_thread.join();
}

void concurrencpp::details::timer_queue::work_loop(){
	while (!m_cancel.load(std::memory_order_acquire)) {
		process_timers();

		std::unique_lock<decltype(m_lock)> lock(m_lock);
		m_condition_variable.wait_for(lock, std::chrono::milliseconds(m_minimum_waiting_time));
	}
}

void concurrencpp::details::timer_queue::process_timers() {
	std::unique_lock<decltype(m_lock)> lock(m_lock);

	if (!static_cast<bool>(m_timers)) { //if there are not timers, bail out
		assert(m_minimum_waiting_time == s_sleep_forever);
		return;
	}

	auto cursor = m_timers;
	auto minimum_fire_time = s_sleep_forever;
	auto callback = [](timer_ptr timer) { timer->execute(); };

	while (static_cast<bool>(cursor)) {
		auto& timer = *cursor;
		const auto status = timer.update();
		const auto next_fire_time = timer.next_fire_time();
		
		if (next_fire_time < minimum_fire_time) {
			if (status != timer_impl::status::SCHEDULE_DELETE) {
				minimum_fire_time = next_fire_time;
			}
		}

		switch (status) {
		case timer_impl::status::SCHEDULE: {
			m_thread_pool.enqueue_task(callback, cursor);
			break;
		}
		case timer_impl::status::SCHEDULE_DELETE: {
			m_thread_pool.enqueue_task(callback, cursor);
			remove_timer(*cursor);
			break;
		}
		}	//end of switch

		cursor = cursor->get_next();
	}	//end of for loop

	m_minimum_waiting_time = minimum_fire_time;
	m_last_time_point = time_from_epoch();
}

void concurrencpp::details::timer_queue::add_timer(timer_ptr timer) noexcept {
	auto timer_fire_time = timer->next_fire_time();
	timer->set_initial_update_time();

	std::lock_guard<decltype(m_lock)> lock(m_lock);
	if (!static_cast<bool>(m_timers)) {
		m_timers = std::move(timer);
	}
	else {
		auto timers = std::move(m_timers);
		m_timers = std::move(timer);
		timers->set_prev(m_timers);
		m_timers->set_next(timers);
	}

	if (timer_fire_time < m_minimum_waiting_time) {
		m_minimum_waiting_time = timer_fire_time;
		m_condition_variable.notify_one();
	}
}

void concurrencpp::details::timer_queue::remove_timer(timer_impl& timer) noexcept {
	std::lock_guard<decltype(m_lock)> lock(m_lock);
	auto prev = timer.get_prev().lock();
	auto next = timer.get_next();

	if (static_cast<bool>(next)) {
		next->set_prev(prev);
	}

	if (static_cast<bool>(prev)) {
		prev->set_next(next);
	}

	if (m_timers.get() == &timer) {
		m_timers = next;
	}

	if (!static_cast<bool>(m_timers)) {
		m_minimum_waiting_time = s_sleep_forever;
	}
}

void concurrencpp::details::schedule_new_timer(std::shared_ptr<timer_impl> timer_ptr) noexcept{
	timer_queue::default_instance().add_timer(std::move(timer_ptr));
}

/*
	OS specific
*/

#include <system_error>

#ifdef CRCPP_WIN_OS

#include <malloc.h>
#include <Windows.h>

void * concurrencpp::details::allocate_aligned(size_t alignment, size_t size) {
	return ::_aligned_malloc(size, alignment);
}

void concurrencpp::details::free_aligned(void * ptr) {
	::_aligned_free(ptr);
}

#endif 

#ifdef CRCPP_UNIX_OS

#include <cstdlib>

void * concurrencpp::details::allocate_aligned(size_t alignment, size_t size) {
	return ::aligned_alloc(alignment, size);
}

void concurrencpp::details::free_aligned(void * ptr) {
	::free(ptr);
}

#endif
