#include "../concurrencpp.h"
#include "concurrencpp_tests.h"

namespace concurrencpp {
	namespace tests {
		void test_thread_pool_enqueue_task_callable();
		void test_thread_pool_enqueue_task_callable_args();
		void test_thread_pool_enqueue_task_lambda();
		void test_thread_pool_enqueue_task_lambda_args();
		void test_thread_pool_enqueue_task_callback();
		void test_thread_pool_raii();
	}
}

void concurrencpp::tests::test_thread_pool_enqueue_task_callable_args() {
	std::atomic_size_t counter = 0;
	auto& thread_pool = concurrencpp::details::thread_pool::default_instance();
	
	for (size_t i = 0; i < 10'000; i++) {
		thread_pool.enqueue_task(helpers::delegate_args(), std::ref(counter));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_thread_pool_enqueue_task_callable() {
	auto& thread_pool = concurrencpp::details::thread_pool::default_instance();
	std::atomic_size_t counter = 0;

	for (size_t i = 0; i < 10'000; i++) {
		thread_pool.enqueue_task(helpers::delegate_no_args(counter));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_thread_pool_enqueue_task_lambda() {
	auto& thread_pool = concurrencpp::details::thread_pool::default_instance();
	std::atomic_size_t counter = 0;

	for (size_t i = 0; i < 10'000; i++) {
		thread_pool.enqueue_task([&counter] {++counter; });
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_thread_pool_enqueue_task_lambda_args() {
	auto& thread_pool = concurrencpp::details::thread_pool::default_instance();
	std::atomic_size_t counter = 0;

	for (size_t i = 0; i < 10'000; i++) {
		thread_pool.enqueue_task([](auto&& counter) {++counter; }, std::ref(counter));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_thread_pool_enqueue_task_callback(){
	auto& thread_pool = concurrencpp::details::thread_pool::default_instance();
	std::atomic_size_t counter = 0;

	for (size_t i = 0; i < 10'000; i++) {
		thread_pool.enqueue_task(concurrencpp::details::make_callback(helpers::delegate_no_args(counter)));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_thread_pool_raii(){
	auto& thread_pool = concurrencpp::details::thread_pool::default_instance();
	helpers::raii_tester tester;

	for (size_t i = 0; i < 10'000; i++) {
		thread_pool.enqueue_task(tester.clone_context());
	}

	std::this_thread::sleep_for(std::chrono::seconds(4));
	tester.test(10'000);
}

void concurrencpp::tests::test_thread_pool() {
	test_thread_pool_enqueue_task_callable();
	test_thread_pool_enqueue_task_callable_args();
	test_thread_pool_enqueue_task_lambda();
	test_thread_pool_enqueue_task_lambda_args();
	test_thread_pool_enqueue_task_callback();
	test_thread_pool_raii();
}