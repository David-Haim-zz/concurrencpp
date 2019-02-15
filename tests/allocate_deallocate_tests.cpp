#include "concurrencpp_tests.h"
#include "../concurrencpp.h"

#include <vector>
#include <chrono>
#include <random>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <thread>

namespace concurrencpp {
	namespace tests {
		class allocation_test_unit {
		private:	
			void* m_memory;
			size_t m_size;
			std::vector<uint8_t> m_data;

			void fill_random_data();
		
		public:
			allocation_test_unit(void* const pointer, size_t size);
			allocation_test_unit(allocation_test_unit&& rhs) noexcept;

			allocation_test_unit& operator = (allocation_test_unit&& rhs) noexcept;

			inline ~allocation_test_unit() { concurrencpp::details::deallocate(m_memory, m_size); }
			
			void operator() () const noexcept;
		};

		void test_allocation_in_range(size_t min_size, size_t max_size, size_t num_of_test_units = 1024);
		void test_allocation_corner_cases();
		void test_allocation_mono_thread_same_bucket();
		void test_allocation_mono_thread();
		void test_allocation_multi_thread();
		void test_allocate_deallocate();
	}
}

void concurrencpp::tests::allocation_test_unit::fill_random_data(){
	m_data.resize(m_size);
	helpers::random randomizer;

	constexpr auto min_val = std::numeric_limits<uint8_t>::min();
	constexpr auto max_val = std::numeric_limits<uint8_t>::max();

	for (auto& c : m_data) {
		c = static_cast<uint8_t>(randomizer(min_val, max_val));
	}

	std::memcpy(m_memory, m_data.data(), m_size);
}

concurrencpp::tests::allocation_test_unit::allocation_test_unit(void * const pointer, size_t size) :
	m_memory(pointer),
	m_size(size) {
	fill_random_data();
}

concurrencpp::tests::allocation_test_unit::allocation_test_unit(allocation_test_unit && rhs) noexcept:
	m_memory(rhs.m_memory),
	m_size(rhs.m_size),
	m_data(std::move(rhs.m_data)) {
	rhs.m_memory = nullptr;
	rhs.m_size = 0;
}

concurrencpp::tests::allocation_test_unit & 
concurrencpp::tests::allocation_test_unit::operator=(allocation_test_unit && rhs) noexcept{
	m_memory = rhs.m_memory;
	m_size = rhs.m_size;
	m_data = std::move(std::move(rhs.m_data));
	rhs.m_memory = nullptr;
	rhs.m_size = 0;
	return *this;
}

void concurrencpp::tests::allocation_test_unit::operator() () const noexcept {
	assert(m_data.size() == m_size);
	assert(m_memory != nullptr);

	const auto same_memory = std::memcmp(m_memory, m_data.data(), m_size) == 0;
	assert_true(same_memory);
}

void concurrencpp::tests::test_allocation_in_range(size_t min_size, size_t max_size, size_t num_of_test_units){
	std::vector<allocation_test_unit> units;
	units.reserve(num_of_test_units);

	helpers::random randomizer;
	for (size_t i = 0; i < num_of_test_units; i++) {
		const auto size = randomizer(min_size, max_size);
		const auto memory = concurrencpp::details::allocate(size);
		units.emplace_back(memory, size);
	}

	//simulate real world case : deallocate 10% of the memory
	//make sure all memory is intact, allocate 10% more memory and repeat	
	const auto _10_precents = num_of_test_units / 10;
	for (auto i = 0; i < 1000; i++) {
		unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();

		std::shuffle(units.begin(), units.end(), std::default_random_engine(seed));

		for (auto j = 0; j < _10_precents; j++) {
			units.pop_back();
		}

		for (const auto& test_unit : units) {
			test_unit();
		}

		for (size_t j = 0; j < _10_precents; j++) {
			const auto size = randomizer(min_size, max_size);
			const auto memory = concurrencpp::details::allocate(size);
			units.emplace_back(memory, size);
		}
	}
}

void concurrencpp::tests::test_allocation_corner_cases(){
	auto memory0 = concurrencpp::details::allocate(0);
	assert_same(memory0, nullptr);
	concurrencpp::details::deallocate(memory0, 0);

	auto memory1 = concurrencpp::details::allocate(std::numeric_limits<size_t>::max());
	assert_same(memory1, nullptr);
	concurrencpp::details::deallocate(memory1, std::numeric_limits<size_t>::max());

	auto memory2 = concurrencpp::details::allocate(1024 + 1);
	assert_not_same(memory2, nullptr);
	concurrencpp::details::deallocate(memory2, 1024 + 1);

	auto memory3 = concurrencpp::details::allocate(1024 * 1024 * 2);
	assert_not_same(memory3, nullptr);
	concurrencpp::details::deallocate(memory3, 1024 * 1024 * 2);
}

void concurrencpp::tests::test_allocation_mono_thread_same_bucket() {
	test_allocation_in_range(0 + 1, 32);
	test_allocation_in_range(32 + 1, 64);
	test_allocation_in_range(64 + 1, 128);
	test_allocation_in_range(128 + 1, 256);
	test_allocation_in_range(256 + 1, 512);
	test_allocation_in_range(512 + 1, 1024);
	test_allocation_in_range(1024 + 1, 1024 * 2);
}

void concurrencpp::tests::test_allocation_mono_thread(){
	test_allocation_in_range(1, 1024 * 2, 1024 * 8);
}

void concurrencpp::tests::test_allocation_multi_thread(){
	const auto num_of_threads = std::thread::hardware_concurrency() * 2;
	auto threads = std::make_unique<helpers::waiting_thread[]>(num_of_threads);

	for (size_t i = 0; i < num_of_threads; i++) {
		threads[i].set_function(test_allocation_mono_thread);
	}

	std::this_thread::sleep_for(std::chrono::seconds(2));

	for (size_t i = 0; i < num_of_threads; i++) {
		threads[i].resume();
	}

	for (size_t i = 0; i < num_of_threads; i++) {
		threads[i].join();
	}
}

void concurrencpp::tests::test_allocate_deallocate(){
	test_allocation_corner_cases();
	test_allocation_mono_thread_same_bucket();
	test_allocation_mono_thread();
	test_allocation_multi_thread();
}