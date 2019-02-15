#include "../concurrencpp.h"
#include "../concurrencpp.h"
#include "concurrencpp_tests.h"

#include <array>

namespace concurrencpp {
	namespace tests {

		std::vector<std::unique_lock<concurrencpp::details::recursive_spinlock>>
			lock_many(concurrencpp::details::recursive_spinlock& lock, size_t times);
		
		void test_recursive_spin_lock_lock_unlock_test_1();
		void test_recursive_spin_lock_lock_unlock_test_2();
	
	}
}

std::vector<std::unique_lock<concurrencpp::details::recursive_spinlock>> concurrencpp::tests::lock_many
	(concurrencpp::details::recursive_spinlock& lock, size_t times){
	std::vector<std::unique_lock<concurrencpp::details::recursive_spinlock>> locks;
	locks.reserve(times);

	for (size_t i = 0; i < times; i++) {
		locks.emplace_back(std::ref(lock));
	}

	return locks;
}

void concurrencpp::tests::test_recursive_spin_lock_lock_unlock_test_1(){
	//In this test, we make sure recursive spinlocking doesn't dead-lock.
	concurrencpp::details::recursive_spinlock lock;
	auto locks = lock_many(lock, 30);
	locks.clear();

	std::unique_lock<decltype(lock)> unique_lock(lock);
}

void concurrencpp::tests::test_recursive_spin_lock_lock_unlock_test_2(){
	concurrencpp::details::recursive_spinlock lock;
	std::array<std::thread, 20> threads;
	size_t counter;

	for (size_t i = 0; i < 10; i++) {
		counter = 0;
		auto time_point = std::chrono::high_resolution_clock::now();
		time_point += std::chrono::seconds(2);

		for (auto& thread : threads) {
			thread = std::thread([&] {
				std::this_thread::sleep_until(time_point);
				auto locks = lock_many(lock, 30);
				++counter;
			});
		}

		for (auto& thread : threads) {
			thread.join();
		}

		auto locks = lock_many(lock, 30);
		assert_same(counter, threads.size());
	}
}

void concurrencpp::tests::test_recursive_spin_lock(){
	test_recursive_spin_lock_lock_unlock_test_1();
	test_recursive_spin_lock_lock_unlock_test_2();
}