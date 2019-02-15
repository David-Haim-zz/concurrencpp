#include "../concurrencpp.h"
#include "concurrencpp_tests.h"

#include <thread>
#include <mutex>
#include <array>
#include <chrono>

namespace concurrencpp {
	namespace tests {
		void test_spin_lock_lock_unlock_test_1();
		void test_spin_lock_lock_unlock_test_2();
		void test_spin_lock_try_lock();
	}
}

void concurrencpp::tests::test_spin_lock_lock_unlock_test_1(){
	concurrencpp::details::spinlock lock;
	std::array<std::thread, 20> threads;
	size_t counter;
	
	for (size_t i = 0; i < 10; i++) {
		counter = 0;
		auto time_point = std::chrono::high_resolution_clock::now();
		time_point += std::chrono::seconds(2);

		for (auto& thread : threads) {
			thread = std::thread([&] {
				std::this_thread::sleep_until(time_point);
				std::unique_lock<concurrencpp::details::spinlock> _lock(lock);
				++counter;
			});
		}
		
		for (auto& thread : threads) {
			thread.join();
		}

		std::unique_lock<concurrencpp::details::spinlock> _lock(lock);
		assert_same(counter, threads.size());
	}

}

void concurrencpp::tests::test_spin_lock_lock_unlock_test_2(){
	std::chrono::time_point<std::chrono::high_resolution_clock> time_point_1, time_point_2;
	concurrencpp::details::spinlock lock;
	std::mutex mutex;	
	std::thread thread;
	constexpr size_t time_to_sleep = 5;

	{
		std::unique_lock<concurrencpp::details::spinlock> _lock(lock);
		thread = std::thread([&] {
			{
				std::unique_lock<std::mutex> mtx_lock(mutex);
				time_point_1 = std::chrono::high_resolution_clock::now();
			}

			{
				std::unique_lock<concurrencpp::details::spinlock> spin_lock(lock);
			}

			{
				std::unique_lock<std::mutex> mtx_lock(mutex);
				time_point_2 = std::chrono::high_resolution_clock::now();
			}

		});

		//lock is released after this line
		std::this_thread::sleep_for(std::chrono::seconds(time_to_sleep));
	}

	thread.join();
	size_t diff = 0;
	{
		std::unique_lock<std::mutex> mtx_lock(mutex);
		diff = std::chrono::duration_cast<std::chrono::seconds>(time_point_2 - time_point_1).count();
	}

	assert_true(diff >= time_to_sleep);
}

void concurrencpp::tests::test_spin_lock_try_lock(){
	//manages to lock on the calling thread
	{
		concurrencpp::details::spinlock lock;
		assert_true(lock.try_lock());
	}

	//manages to lock on a different thread
	{
		concurrencpp::details::spinlock lock;
		std::atomic_bool locked = false;
		std::thread thread([&] {
			locked = lock.try_lock();
		});
		thread.join();
		assert_true(locked);
	}

	//can't lock, then can
	{
		concurrencpp::details::spinlock lock;
		std::atomic_bool locked = false;

		{
			std::unique_lock<concurrencpp::details::spinlock> _lock(lock);
			std::thread thread([&] {
				locked = lock.try_lock();
			});
			thread.join();
		}

		assert_false(locked);

		{
			std::thread thread([&] {
				locked = lock.try_lock();
			});
			thread.join();
		}
		assert_true(locked);

	}
}

void concurrencpp::tests::test_spin_lock(){
	test_spin_lock_lock_unlock_test_1();
	test_spin_lock_lock_unlock_test_2();
	test_spin_lock_try_lock();
}