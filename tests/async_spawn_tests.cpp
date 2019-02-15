#include "concurrencpp_tests.h"
#include "../concurrencpp.h"

#include <string>

namespace concurrencpp {
	namespace tests {
		void test_spawn_callable();
		void test_spawn_callable_args();
		void test_spawn_raii();
		void test_spawn();

		void test_spawn_blocked_callable();
		void test_spawn_blocked_callable_args();
		void test_spawn_blocked_raii();
		void test_spawn_blocked();

		template<class type>
		void test_async_val_impl(concurrencpp::launch launch_policy);
		void test_async_val();

		template<class type>
		void test_async_ex_impl(concurrencpp::launch launch_policy, const size_t id);
		void test_async_ex();

		template<class type>
		void test_async_deffered_impl_val();

		template<class type>
		void test_async_deffered_impl_ex(const size_t id);
		void test_async_deffered();

		void test_async_raii_imp(concurrencpp::launch policy);
		void test_async_raii();

		void test_async();
	}
}

void concurrencpp::tests::test_spawn_callable() {
	std::atomic_size_t counter = 0;
	for (size_t i = 0; i < 10'000; i++) {
		concurrencpp::spawn(helpers::delegate_no_args(counter));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_spawn_callable_args(){
	std::atomic_size_t counter = 0;
	for (size_t i = 0; i < 10'000; i++) {
		concurrencpp::spawn(helpers::delegate_args(), std::ref(counter));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_spawn_raii(){
	helpers::raii_tester tester;

	for (size_t i = 0; i < 10'000; i++) {
		concurrencpp::spawn(tester.clone_context());
	}

	std::this_thread::sleep_for(std::chrono::seconds(4));
	tester.test(10'000);
}

void concurrencpp::tests::test_spawn() {
	test_spawn_callable();
	test_spawn_callable_args();
	test_spawn_raii();
}

void concurrencpp::tests::test_spawn_blocked_callable() {
	std::atomic_size_t counter = 0;
	for (size_t i = 0; i < 10'000; i++) {
		concurrencpp::spawn_blocked(helpers::delegate_no_args(counter));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_spawn_blocked_callable_args() {
	std::atomic_size_t counter = 0;
	for (size_t i = 0; i < 10'000; i++) {
		concurrencpp::spawn_blocked(helpers::delegate_args(), std::ref(counter));
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, 10'000);
}

void concurrencpp::tests::test_spawn_blocked_raii() {
	helpers::raii_tester tester;

	for (size_t i = 0; i < 10'000; i++) {
		concurrencpp::spawn_blocked(tester.clone_context());
	}

	std::this_thread::sleep_for(std::chrono::seconds(4));
	tester.test(10'000);
}

void concurrencpp::tests::test_spawn_blocked() {
	test_spawn_blocked_callable();
	test_spawn_blocked_callable_args();
	test_spawn_blocked_raii();
}

template<class type>
void concurrencpp::tests::test_async_val_impl(concurrencpp::launch launch_policy){
	auto action = []() -> decltype(auto) {
		std::this_thread::sleep_for(std::chrono::seconds(2));
		return  helpers::result_factory<type>::get();
	};
	
	auto future = ::concurrencpp::async(launch_policy, action);
	future.wait();
	helpers::test_ready_future_result(std::move(future));
}

void concurrencpp::tests::test_async_val(){
	test_async_val_impl<int>(concurrencpp::launch::async);
	test_async_val_impl<int>(concurrencpp::launch::task);
	test_async_val_impl<int>(concurrencpp::launch::blocked_task);
	test_async_val_impl<int>(concurrencpp::launch::deferred);

	test_async_val_impl<std::string>(concurrencpp::launch::async);
	test_async_val_impl<std::string>(concurrencpp::launch::task);
	test_async_val_impl<std::string>(concurrencpp::launch::blocked_task);
	test_async_val_impl<std::string>(concurrencpp::launch::deferred);

	test_async_val_impl<void>(concurrencpp::launch::async);
	test_async_val_impl<void>(concurrencpp::launch::task);
	test_async_val_impl<void>(concurrencpp::launch::blocked_task);
	test_async_val_impl<void>(concurrencpp::launch::deferred);

	test_async_val_impl<int&>(concurrencpp::launch::async);
	test_async_val_impl<int&>(concurrencpp::launch::task);
	test_async_val_impl<int&>(concurrencpp::launch::blocked_task);
	test_async_val_impl<int&>(concurrencpp::launch::deferred);

	test_async_val_impl<std::string&>(concurrencpp::launch::async);
	test_async_val_impl<std::string&>(concurrencpp::launch::task);
	test_async_val_impl<std::string&>(concurrencpp::launch::blocked_task);
	test_async_val_impl<std::string&>(concurrencpp::launch::deferred);
}

template<class type>
void concurrencpp::tests::test_async_ex_impl(concurrencpp::launch launch_policy, const size_t id){
	auto action = [id]() -> decltype(auto) {
		std::this_thread::sleep_for(std::chrono::seconds(2));
		throw helpers::costume_exception(id);
		return helpers::result_factory<type>::get();
	};

	auto future = ::concurrencpp::async(launch_policy, action);
	future.wait();
	helpers::test_ready_future_costume_exception(std::move(future), id);
}

void concurrencpp::tests::test_async_ex(){
	helpers::random random;

	test_async_ex_impl<int>(concurrencpp::launch::async, random());
	test_async_ex_impl<int>(concurrencpp::launch::task, random());
	test_async_ex_impl<int>(concurrencpp::launch::blocked_task, random());
	test_async_ex_impl<int>(concurrencpp::launch::deferred, random());

	test_async_ex_impl<std::string>(concurrencpp::launch::async, random());
	test_async_ex_impl<std::string>(concurrencpp::launch::task, random());
	test_async_ex_impl<std::string>(concurrencpp::launch::blocked_task, random());
	test_async_ex_impl<std::string>(concurrencpp::launch::deferred, random());

	test_async_ex_impl<void>(concurrencpp::launch::async, random());
	test_async_ex_impl<void>(concurrencpp::launch::task, random());
	test_async_ex_impl<void>(concurrencpp::launch::blocked_task, random());
	test_async_ex_impl<void>(concurrencpp::launch::deferred, random());

	test_async_ex_impl<int&>(concurrencpp::launch::async, random());
	test_async_ex_impl<int&>(concurrencpp::launch::task, random());
	test_async_ex_impl<int&>(concurrencpp::launch::blocked_task, random());
	test_async_ex_impl<int&>(concurrencpp::launch::deferred, random());

	test_async_ex_impl<std::string&>(concurrencpp::launch::async, random());
	test_async_ex_impl<std::string&>(concurrencpp::launch::task, random());
	test_async_ex_impl<std::string&>(concurrencpp::launch::blocked_task, random());
	test_async_ex_impl<std::string&>(concurrencpp::launch::deferred, random());
}

template<class type>
void concurrencpp::tests::test_async_deffered_impl_val(){
	std::atomic<std::thread::id> executor_id = {};
	std::atomic_bool executed = false;
	auto action = [&]() -> decltype(auto) {
		executor_id = std::this_thread::get_id();
		executed = true;
		return helpers::result_factory<type>::get();
	};

	auto future = concurrencpp::async(concurrencpp::launch::deferred, action);

	assert_false(executed);
	assert_same(executor_id.load(), std::thread::id());

	future.get();

	assert_true(executed);
	assert_same(executor_id.load(), std::this_thread::get_id());
}

template<class type>
void concurrencpp::tests::test_async_deffered_impl_ex(const size_t id) {
	std::atomic<std::thread::id> executor_id = {};
	std::atomic_bool executed = false;

	auto action = [&, id]() -> decltype(auto) {
		executor_id = std::this_thread::get_id();
		executed = true;
		throw helpers::costume_exception(id);
		return helpers::result_factory<type>::get();
	};

	auto future = concurrencpp::async(concurrencpp::launch::deferred, action);

	assert_false(executed);
	assert_same(executor_id.load(), std::thread::id());

	try {
		future.get();
	}
	catch (helpers::costume_exception e) {
		assert_same(e.id, id);
		assert_true(executed);
		assert_same(executor_id.load(), std::this_thread::get_id());
		return;
	}
	catch (...) {}

	assert_false(true);
}

void concurrencpp::tests::test_async_deffered(){
	test_async_deffered_impl_val<int>();
	test_async_deffered_impl_val<std::string>();
	test_async_deffered_impl_val<void>();
	test_async_deffered_impl_val<int&>();
	test_async_deffered_impl_val<std::string&>();

	helpers::random random;
	test_async_deffered_impl_ex<int>(random());
	test_async_deffered_impl_ex<std::string>(random());
	test_async_deffered_impl_ex<void>(random());
	test_async_deffered_impl_ex<int&>(random());
	test_async_deffered_impl_ex<std::string&>(random());
}

void concurrencpp::tests::test_async_raii_imp(concurrencpp::launch policy) {
	helpers::raii_tester tester;

	std::vector<concurrencpp::future<void>> futures;
	futures.reserve(10'000);
	
	for (size_t i = 0; i < 10'000; i++) {
		futures.emplace_back(concurrencpp::async(policy, tester.clone_context()));
	}

	for (auto& future : futures) {
		future.get();
	}

	futures.clear();
	tester.test(10'000);
}

void concurrencpp::tests::test_async_raii() {
	test_async_raii_imp(concurrencpp::launch::deferred);
	test_async_raii_imp(concurrencpp::launch::async);
	test_async_raii_imp(concurrencpp::launch::task);
	test_async_raii_imp(concurrencpp::launch::blocked_task);
	test_async_raii_imp(concurrencpp::launch::deferred);
}

void concurrencpp::tests::test_async(){
	test_async_val();
	test_async_ex();
	test_async_deffered();
	test_async_raii();	
}

void concurrencpp::tests::test_spawn_async() {
	test_spawn();
	test_spawn_blocked();
	test_async();
}