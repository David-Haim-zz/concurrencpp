#include "concurrencpp_tests.h"
#include "../concurrencpp.h"

#include <array>
#include <tuple>
#include <thread>
#include <chrono>

namespace concurrencpp {
	namespace tests {

		template<class type, class exception_type, class action_type, class exception_comparator>
		void test_invalid_future(action_type&& action, exception_comparator&& comp);

		template<class type, bool is_for>
		std::future_status wait_for_until(future<type>& future, std::chrono::seconds seconds);

		template<class type>
		void test_future_valid_impl();
		void test_future_valid();

		template<class type>
		void test_future_get_impl();
		void test_future_get();

		template<class type>
		void test_future_wait_deffered();

		template<class type>
		void test_future_wait_val_impl();

		template<class type>
		void test_future_wait_err_impl();

		void test_future_wait();

		template<class type, bool is_wait_for>
		void test_future_wait_for_until_timeout_impl();

		template<class type, bool is_wait_for>
		void test_future_wait_for_until_val_impl();

		template<class type, bool is_wait_for>
		void test_future_wait_for_until_err_impl();

		template<class type, bool is_wait_for>
		void test_future_wait_for_until_deffered_impl();

		void test_future_wait_for();

		void test_future_wait_until();

		//then:
		template<class type>
		void test_future_then_ready_val();

		template<class type>
		void test_future_then_ready_err();

		template<class type>
		void test_future_then_value_impl();

		void test_future_then_value_void_impl();

		template<class type>
		void test_future_then_exception_impl();

		template<class type>
		void test_future_then_raii_impl();

		void test_future_then_raii();

		void test_future_then();

		void test_make_ready_future();

		template<class type>
		void test_make_exceptional_future_impl(helpers::random& randomizer);
		void test_make_exceptional_future();

		void test_when_all_empty();
		void test_when_all_many();
		void test_when_all();

		template<size_t index>
		void test_when_any_many(helpers::random& randomizer);
		void test_when_any_empty();
		void test_when_any();
	}
}

namespace concurrencpp {
	namespace tests {
		class then_raii_wrapper {
		private:
			std::atomic_size_t* m_counter;

		public:
			then_raii_wrapper(decltype(m_counter) counter) noexcept : m_counter(counter) {}
			then_raii_wrapper(then_raii_wrapper&& rhs) noexcept { m_counter = rhs.m_counter; rhs.m_counter = nullptr; }
			~then_raii_wrapper() { if (m_counter != nullptr) { ++(*m_counter); } }
			template<class type> void operator()(::concurrencpp::future<type> future) const noexcept {}
		};
	}
}

template<class type, class exception_type, class action_type, class exception_comparator>
void concurrencpp::tests::test_invalid_future(action_type && action, exception_comparator&& comp) {
	/*
	   An invalid future is a future without state, aka
		1) a defaultly created future
		2) a moved future
		3) a retrieved future
	*/

	concurrencpp::future<type> default_constructed_future;
	concurrencpp::promise<type> promise_a, promise_b;
	auto moved_future = promise_a.get_future();
	concurrencpp::future<type> ignored(std::move(moved_future));

	promise_b.set_exception(std::make_exception_ptr(std::exception()));
	auto retrieved_future = promise_b.get_future();

	try {
		retrieved_future.get();
	}
	catch (...) {}

	assert_throws<exception_type>([&] {action(default_constructed_future); }, comp);
	assert_throws<exception_type>([&] {action(moved_future); }, comp);
	assert_throws<exception_type>([&] {action(retrieved_future); }, comp);
}

template<class type, bool is_for>
std::future_status concurrencpp::tests::wait_for_until(future<type>& future, std::chrono::seconds seconds){
	if (is_for) {
		return future.wait_for(seconds);
	}

	const auto time_point = std::chrono::system_clock::now() + seconds;
	return future.wait_until(time_point);
}

template<class type>
void concurrencpp::tests::test_future_valid_impl(){
	/*
		An invalid future is
			1) a defaultly created future
			2) a moved future
			3) a retrieved future
	*/

	concurrencpp::future<type> default_constructed;
	concurrencpp::promise<type> promise_a, promise_b;
	auto moved_future = promise_a.get_future();
	concurrencpp::future<type> ignored(std::move(moved_future));

	promise_b.set_exception(std::make_exception_ptr(std::exception()));
	auto retrieved_future = promise_b.get_future();

	try {
		retrieved_future.get();
	}
	catch (...) {}

	assert_false(default_constructed.valid());
	assert_false(moved_future.valid());
	assert_false(retrieved_future.valid());

	details::unsafe_promise<type> valid_promise;
	auto valid_future = valid_promise.get_future();
	assert_true(valid_future.valid());
}

void concurrencpp::tests::test_future_valid() {
	test_future_valid_impl<int>();
	test_future_valid_impl<std::string>();
	test_future_valid_impl<void>();
	test_future_valid_impl<int&>();
	test_future_valid_impl<std::string&>();
}

template<class type>
void concurrencpp::tests::test_future_get_impl(){
	//invalid future throws
	{
		auto action = [](auto& future) { future.get(); };
		auto comparator = [](auto e) { assert_same(e.code(), std::future_errc::no_state); };

		test_invalid_future<type, std::future_error>(action, comparator);
	}

	//future::get blocks the thread until result is set
	{
		std::atomic_bool unblocked = false;
		concurrencpp::promise<type> promise;
		auto future = promise.get_future();
		std::thread thread([future = std::move(future), &unblocked]() mutable{
			future.get();
			unblocked = true;
		});

		for (size_t i = 0; i < 10; i++) {
			assert_false(unblocked.load());
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

		tests::helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		assert_true(unblocked.load());
		thread.join();
	}

	//future::get blocks the thread until exception is set
	{
		std::atomic_bool unblocked = false;
		concurrencpp::promise<type> promise;
		auto future = promise.get_future();
		std::thread thread([future = std::move(future), &unblocked]() mutable{
			try {
				future.get();
			}
			catch (...) {}
			unblocked = true;
		});

		for (size_t i = 0; i < 10; i++) {
			assert_false(unblocked.load());
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

		promise.set_exception(std::make_exception_ptr(std::exception()));
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		assert_true(unblocked.load());
		thread.join();
	}

	//future::get returns the result set by promise::set_value
	{
		concurrencpp::promise<type> promise;
		auto future = promise.get_future();
		tests::helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);
		
		helpers::test_ready_future_result(std::move(future));
	}

	//future::get re-throws the exception set by promise::set_exception
	{
		helpers::random randomizer;
		const auto id = randomizer();
		concurrencpp::promise<type> promise;
		auto future = promise.get_future();
		promise.set_exception(std::make_exception_ptr(helpers::costume_exception(id)));

		helpers::test_ready_future_costume_exception(std::move(future), id);
	}
}

void concurrencpp::tests::test_future_get() {
	test_future_get_impl<int>();
	test_future_get_impl<std::string>();
	test_future_get_impl<void>();
	test_future_get_impl<int&>();
	test_future_get_impl<std::string&>();
}

template<class type>
void concurrencpp::tests::test_future_wait_deffered() {
	std::atomic<std::thread::id> thread_id = {};
	auto action = [&thread_id]() -> decltype(auto) {
		thread_id = std::this_thread::get_id();
		return helpers::result_factory<type>::get();
	};

	auto future = ::concurrencpp::async(::concurrencpp::launch::deferred, action);
	assert_same(thread_id, std::thread::id{});

	future.wait();

	assert_same(thread_id, std::this_thread::get_id());
	helpers::test_ready_future_result(std::move(future));
}

template<class type>
void concurrencpp::tests::test_future_wait_val_impl() {
	std::atomic_bool thread_blocked = true;
	::concurrencpp::promise<type> promise;
	auto future = promise.get_future();

	std::thread thread([future = std::move(future), &thread_blocked]() mutable {
		future.wait();
		thread_blocked = false;
		helpers::test_ready_future_result(std::move(future));
	});

	for (size_t i = 0; i < 5; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		assert_true(thread_blocked);
	}

	helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);

	thread.join();
	assert_false(thread_blocked);
}

template<class type>
void concurrencpp::tests::test_future_wait_err_impl() {
	std::atomic_bool thread_blocked = true;
	::concurrencpp::promise<type> promise;
	auto future = promise.get_future();
	helpers::random randomizer;
	const auto id = randomizer();

	std::thread thread([future = std::move(future), id, &thread_blocked]() mutable {
		future.wait();
		thread_blocked = false;
		helpers::test_ready_future_costume_exception(std::move(future), id);
	});

	for (size_t i = 0; i < 5; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		assert_true(thread_blocked);
	}

	promise.set_exception(std::make_exception_ptr<helpers::costume_exception>(id));
	thread.join();
	assert_false(thread_blocked);
}

void concurrencpp::tests::test_future_wait() {
	auto action = [](auto& future) { future.wait(); };
	auto comp = [](auto e) { assert_same(e.code(), std::future_errc::no_state); };

	test_invalid_future<int, std::future_error>(action, comp);
	test_invalid_future<std::string, std::future_error>(action, comp);
	test_invalid_future<void, std::future_error>(action, comp);
	test_invalid_future<int&, std::future_error>(action, comp);
	test_invalid_future<std::string&, std::future_error>(action, comp);

	test_future_wait_val_impl<int>();
	test_future_wait_val_impl<std::string>();
	test_future_wait_val_impl<void>();
	test_future_wait_val_impl<int&>();
	test_future_wait_val_impl<std::string&>();

	test_future_wait_err_impl<int>();
	test_future_wait_err_impl<std::string>();
	test_future_wait_err_impl<void>();
	test_future_wait_err_impl<int&>();
	test_future_wait_err_impl<std::string&>();

	test_future_wait_deffered<int>();
	test_future_wait_deffered<std::string>();
	test_future_wait_deffered<void>();
	test_future_wait_deffered<int&>();
	test_future_wait_deffered<std::string&>();
}

template<class type, bool is_wait_for>
void concurrencpp::tests::test_future_wait_for_until_timeout_impl() {
	std::atomic<std::future_status> status = std::future_status::deferred;
	auto action = [&status](::concurrencpp::future<type> future) mutable {
		status = wait_for_until<type, is_wait_for>(future, std::chrono::seconds(5));
	};

	::concurrencpp::promise<type> promise;
	std::thread thread(action, promise.get_future());

	for (size_t i = 0; i < 10; i++) {
		assert_same(status, std::future_status::deferred);
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
	}

	thread.join();
	assert_same(status, std::future_status::timeout);
}

template<class type, bool is_wait_for>
void concurrencpp::tests::test_future_wait_for_until_val_impl() {
	std::atomic<std::future_status> status = std::future_status::deferred;
	auto action = [&status](::concurrencpp::future<type> future) mutable {
		status = wait_for_until<type, is_wait_for>(future, std::chrono::seconds(20));
		helpers::test_ready_future_result(std::move(future));
	};

	::concurrencpp::promise<type> promise;
	std::thread thread(action, promise.get_future());

	for (size_t i = 0; i < 10; i++) {
		assert_same(status, std::future_status::deferred);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	helpers::promise_setter::execute_set_promise(promise, &helpers::result_factory<type>::get);
	thread.join();
	assert_same(status, std::future_status::ready);
}

template<class type, bool is_wait_for>
void concurrencpp::tests::test_future_wait_for_until_err_impl() {
	helpers::random randomizer;
	const auto id = randomizer();
	std::atomic<std::future_status> status = std::future_status::deferred;
	auto action = [&status, id](::concurrencpp::future<type> future) mutable {
		status = wait_for_until<type, is_wait_for>(future, std::chrono::seconds(5));
		helpers::test_ready_future_costume_exception(std::move(future), id);
	};

	::concurrencpp::promise<type> promise;
	std::thread thread(action, promise.get_future());

	for (size_t i = 0; i < 10; i++) {
		assert_same(status, std::future_status::deferred);
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
	}

	promise.set_exception(std::make_exception_ptr(helpers::costume_exception(id)));
	thread.join();
	assert_same(status, std::future_status::ready);
}

template<class type, bool is_wait_for>
void concurrencpp::tests::test_future_wait_for_until_deffered_impl() {
	auto future = ::concurrencpp::async(::concurrencpp::launch::deferred, helpers::result_factory<type>::get);

	auto tp1 =  concurrencpp::details::time_from_epoch();
	auto status = wait_for_until<type, is_wait_for>(future, std::chrono::seconds(5)); //non blocking.
	auto tp2 = concurrencpp::details::time_from_epoch();

	assert_same(status, std::future_status::deferred);
	assert_true(tp2 - tp1 <= 1);
}

void concurrencpp::tests::test_future_wait_for() {
	auto action = [](auto& future) { future.wait_for(std::chrono::milliseconds(100)); };
	auto comp = [](auto e) { assert_same(e.code(), std::future_errc::no_state); };

	test_invalid_future<int, std::future_error>(action, comp);
	test_invalid_future<std::string, std::future_error>(action, comp);
	test_invalid_future<void, std::future_error>(action, comp);
	test_invalid_future<int&, std::future_error>(action, comp);
	test_invalid_future<std::string&, std::future_error>(action, comp);

	//timeout
	test_future_wait_for_until_timeout_impl<int, true>();
	test_future_wait_for_until_timeout_impl<std::string, true>();
	test_future_wait_for_until_timeout_impl<void, true>();
	test_future_wait_for_until_timeout_impl<int&, true>();
	test_future_wait_for_until_timeout_impl<std::string&, true>();

	//result
	test_future_wait_for_until_val_impl<int, true>();
	test_future_wait_for_until_val_impl<std::string, true>();
	test_future_wait_for_until_val_impl<void, true>();
	test_future_wait_for_until_val_impl<int&, true>();
	test_future_wait_for_until_val_impl<std::string&, true>();

	//exception
	test_future_wait_for_until_err_impl<int, true>();
	test_future_wait_for_until_err_impl<std::string, true>();
	test_future_wait_for_until_err_impl<void, true>();
	test_future_wait_for_until_err_impl<int&, true>();
	test_future_wait_for_until_err_impl<std::string&, true>();

	//deffered
	test_future_wait_for_until_deffered_impl<int, true>();
	test_future_wait_for_until_deffered_impl<std::string, true>();
	test_future_wait_for_until_deffered_impl<void, true>();
	test_future_wait_for_until_deffered_impl<int&, true>();
	test_future_wait_for_until_deffered_impl<std::string&, true>();
}

void concurrencpp::tests::test_future_wait_until(){
	auto action = [](auto& future) {		
		auto time_point = std::chrono::system_clock::now() + std::chrono::microseconds(100);
		future.wait_until(time_point); 
	};

	auto comp = [](auto e) { assert_same(e.code(), std::future_errc::no_state); };

	test_invalid_future<int, std::future_error>(action, comp);
	test_invalid_future<std::string, std::future_error>(action, comp);
	test_invalid_future<void, std::future_error>(action, comp);
	test_invalid_future<int&, std::future_error>(action, comp);
	test_invalid_future<std::string&, std::future_error>(action, comp);

	//timeout
	test_future_wait_for_until_timeout_impl<int, false>();
	test_future_wait_for_until_timeout_impl<std::string, false>();
	test_future_wait_for_until_timeout_impl<void, false>();
	test_future_wait_for_until_timeout_impl<int&, false>();
	test_future_wait_for_until_timeout_impl<std::string&, false>();

	//result
	test_future_wait_for_until_val_impl<int, false>();
	test_future_wait_for_until_val_impl<std::string, false>();
	test_future_wait_for_until_val_impl<void, false>();
	test_future_wait_for_until_val_impl<int&, false>();
	test_future_wait_for_until_val_impl<std::string&, false>();

	//exception
	test_future_wait_for_until_err_impl<int, false>();
	test_future_wait_for_until_err_impl<std::string, false>();
	test_future_wait_for_until_err_impl<void, false>();
	test_future_wait_for_until_err_impl<int&, false>();
	test_future_wait_for_until_err_impl<std::string&, false>();

	//deffered
	test_future_wait_for_until_deffered_impl<int, false>();
	test_future_wait_for_until_deffered_impl<std::string, false>();
	test_future_wait_for_until_deffered_impl<void, false>();
	test_future_wait_for_until_deffered_impl<int&, false>();
	test_future_wait_for_until_deffered_impl<std::string&, false>();
}

template<class type>
void concurrencpp::tests::test_future_then_ready_val() {
	::concurrencpp::promise<type> promise;
	helpers::promise_setter::execute_set_promise(promise, &helpers::result_factory<type>::get);

	auto future = promise.get_future();
	auto this_thread_id = std::this_thread::get_id();
	auto action = [this_thread_id](auto future) {
		assert_same(std::this_thread::get_id(), this_thread_id);
		helpers::test_ready_future_result(std::move(future));
	};

	future.then(action);
	assert_false(future.valid());
}

template<class type>
void concurrencpp::tests::test_future_then_ready_err() {
	::concurrencpp::promise<type> promise;
	helpers::random randomizer;
	const auto err_id = randomizer();
	promise.set_exception(std::make_exception_ptr(helpers::costume_exception(err_id)));

	auto future = promise.get_future();
	auto this_thread_id = std::this_thread::get_id();
	auto action = [this_thread_id, err_id](auto future) {
		assert_same(std::this_thread::get_id(), this_thread_id);
		try {
			future.get();
		}
		catch (helpers::costume_exception e) {
			assert_same(e.id, err_id);
			return;
		}
		catch (...) {}
		assert_true(false);
	};

	future.then(action);
	assert_false(future.valid());
}

template<class type>
void concurrencpp::tests::test_future_then_value_impl() {
	/*
		In this test we test 2 things
		a) that the value passed to promise::set_value is the value we get in the then callback (by calling future::get)
		b) that the thread which sets the result is the thread which executed the callback.
	*/
	
	helpers::waiting_thread thread;
	concurrencpp::promise<type> promise;
	auto future = promise.get_future();
	const auto expected_thread_id = thread.get_id();

	future.then([expected_thread_id](::concurrencpp::future<type> done_future) {
		assert_same(std::this_thread::get_id(), expected_thread_id);
		helpers::test_ready_future_result(std::move(done_future));
	});

	thread.set_function([promise = std::move(promise)]() mutable {
		helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);
	});

	thread.resume();
	thread.join();
}

template<class type>
void concurrencpp::tests::test_future_then_exception_impl() {
	/*
		In this test we test 2 things
		a) that the exception passed to promise::set_Exception is the value we get in the then callback (by calling future::get)
		b) that the thread which sets the result is the thread which executed the callback.
	*/

	helpers::waiting_thread thread;
	concurrencpp::promise<type> promise;
	auto future = promise.get_future();
	const auto expected_thread_id = thread.get_id();
	helpers::random randomizer;
	const auto exception_id = randomizer();

	future.then([expected_thread_id, exception_id](::concurrencpp::future<type> done_future) {
		assert_same(std::this_thread::get_id(), expected_thread_id);
		helpers::test_ready_future_costume_exception(std::move(done_future), exception_id);
	});

	thread.set_function([promise = std::move(promise), exception_id]() mutable {
		promise.set_exception(std::make_exception_ptr(helpers::costume_exception(exception_id)));
	});

	thread.resume();
	thread.join();
}

template<class type>
void concurrencpp::tests::test_future_then_raii_impl(){
	std::atomic_size_t counter = 0;

	for (size_t i = 0; i < 1'000; i++) {
		::concurrencpp::promise<type> promise;
		auto future = promise.get_future();
		future.then(then_raii_wrapper(&counter));
		auto before = counter.load();

		std::thread thread([promise = std::move(promise)] () mutable {
			helpers::promise_setter::execute_set_promise(promise, [] ()->type{
				return helpers::result_factory<type>::get();
			});
		});

		thread.join();
		
		assert_same(counter, before + 1);
	}
}

void concurrencpp::tests::test_future_then_raii() {
	test_future_then_raii_impl<int>();
	test_future_then_raii_impl<std::string>();
	test_future_then_raii_impl<void>();
	test_future_then_raii_impl<int&>();
	test_future_then_raii_impl<std::string&>();
}

void concurrencpp::tests::test_future_then() {
	//Invalid future
	auto action = [](auto& future) { future.then([](auto future_) {}); };
	auto comp = [](auto e) { assert_same(e.code(), std::future_errc::no_state); };

	test_invalid_future<int, std::future_error>(action, comp);
	test_invalid_future<std::string, std::future_error>(action, comp);
	test_invalid_future<void, std::future_error>(action, comp);
	test_invalid_future<int&, std::future_error>(action, comp);
	test_invalid_future<std::string&, std::future_error>(action, comp);

	//ready - result.
	test_future_then_ready_val<int>();
	test_future_then_ready_val<std::string>();
	test_future_then_ready_val<void>();
	test_future_then_ready_val<int&>();
	test_future_then_ready_val<std::string&>();

	//ready - exception
	test_future_then_ready_err<int>();
	test_future_then_ready_err<std::string>();
	test_future_then_ready_err<void>();
	test_future_then_ready_err<int&>();
	test_future_then_ready_err<std::string&>();

	//not ready - value
	test_future_then_value_impl<int>();
	test_future_then_value_impl<std::string>();
	test_future_then_value_impl<void>();
	test_future_then_value_impl<int&>();
	test_future_then_value_impl<std::string&>();

	//not ready - err
	test_future_then_exception_impl<int>();
	test_future_then_exception_impl<std::string>();
	test_future_then_exception_impl<void>();
	test_future_then_exception_impl<int&>();
	test_future_then_exception_impl<std::string&>();

	test_future_then_raii();
}

//TODO
void concurrencpp::tests::test_make_ready_future() {
	auto assert_future_ok = [](auto& future, auto result) {
		assert_true(future.valid());
		assert_same(future.wait_for(::std::chrono::seconds(0)), std::future_status::ready);
		assert_same(future.get(), result);
	};

	auto assert_future_ref_ok = [](auto& future, auto& result) {
		assert_true(future.valid());
		assert_same(future.wait_for(::std::chrono::seconds(0)), std::future_status::ready);
		assert_same(&future.get(), &result);
	};

	auto assert_future_void_ok = [](auto& future) {
		assert_true(future.valid());
		assert_same(future.wait_for(::std::chrono::seconds(0)), std::future_status::ready);
	};


	auto f0 = ::concurrencpp::make_ready_future<int>();
	assert_future_ok(f0, 0);

	auto f1 = ::concurrencpp::make_ready_future<int>(1234567);
	assert_future_ok(f1, 1234567);

	auto f2 = ::concurrencpp::make_ready_future<std::string>();
	assert_future_ok(f2, "");

	auto f3 = ::concurrencpp::make_ready_future<std::string>("hello world");
	assert_future_ok(f3, "hello world");

	auto f4 = ::concurrencpp::make_ready_future<std::string>("hello world       ", 11);
	assert_future_ok(f4, "hello world");

	auto f5 = ::concurrencpp::make_ready_future<void>();
	assert_future_void_ok(f5);

	int integer = 0;
	auto f6 = ::concurrencpp::make_ready_future<int&>(integer);
	assert_future_ref_ok(f6, integer);

	std::string string;
	auto f7 = ::concurrencpp::make_ready_future<std::string&>(string);
	assert_future_ref_ok(f7, string);
}

template<class type>
void concurrencpp::tests::test_make_exceptional_future_impl(helpers::random& randomizer){	
	const auto id = randomizer();
	auto exception_ptr = std::make_exception_ptr<helpers::costume_exception>(id);
	auto future = ::concurrencpp::make_exceptional_future<type>(exception_ptr);
	helpers::test_ready_future_costume_exception(std::move(future), id);
}

void concurrencpp::tests::test_make_exceptional_future() {
	helpers::random randomizer;
	test_make_exceptional_future_impl<int>(randomizer);
	test_make_exceptional_future_impl<std::string>(randomizer);
	test_make_exceptional_future_impl<void>(randomizer);
	test_make_exceptional_future_impl<int&>(randomizer);
	test_make_exceptional_future_impl<std::string&>(randomizer);
}

void concurrencpp::tests::test_when_all_empty(){
	::concurrencpp::future<::std::tuple<>> future = concurrencpp::when_all();
	assert_true(future.valid());
	assert_same(future.wait_for(::std::chrono::seconds(0)), std::future_status::ready);
	auto res = future.get(); //doesn't throw.
	assert_same(std::tuple_size<decltype(res)>(), 0);
}

void concurrencpp::tests::test_when_all_many(){
	auto is_ready = [](const auto& future) {return future.wait_for(std::chrono::seconds(0)) != std::future_status::timeout; };
	auto make_costume_exception = [](auto id) {return ::std::make_exception_ptr(helpers::costume_exception(id)); };
	helpers::random randomizer;

	::concurrencpp::promise<int> promise_int_val, promise_int_err;
	::concurrencpp::promise<std::string> promise_str_val, promise_str_err;
	::concurrencpp::promise<void> promise_void_val, promise_void_err;
	::concurrencpp::promise<int&> promise_int_ref_val, promise_int_ref_err;
	::concurrencpp::promise<std::string&> promise_str_ref_val, promise_str_ref_err;

	auto int_val = promise_int_val.get_future(), int_err = promise_int_err.get_future();
	auto str_val = promise_str_val.get_future(), str_err = promise_str_err.get_future();
	auto void_val = promise_void_val.get_future(), void_err = promise_void_err.get_future();
	auto int_ref_val = promise_int_ref_val.get_future(), int_ref_err = promise_int_ref_err.get_future();
	auto str_ref_val = promise_str_ref_val.get_future(), str_ref_err = promise_str_ref_err.get_future();

	auto when_all_future = ::concurrencpp::when_all(
		int_val, int_err,
		str_val, str_err,
		void_val, void_err,
		int_ref_val, int_ref_err,
		str_ref_val, str_ref_err);

	assert_false(int_val.valid());
	assert_false(int_err.valid());
	assert_false(str_val.valid());
	assert_false(str_err.valid());
	assert_false(int_ref_val.valid());
	assert_false(int_ref_err.valid());
	assert_false(str_ref_val.valid());
	assert_false(str_ref_err.valid());
	assert_false(void_val.valid());
	assert_false(void_err.valid());
	
	assert_true(when_all_future.valid());
	assert_false(is_ready(when_all_future));

	//set int futures:
	promise_int_val.set_value(helpers::result_factory<int>::get());
	assert_false(is_ready(when_all_future));

	const size_t int_err_id = randomizer();
	promise_int_err.set_exception(make_costume_exception(int_err_id));
	assert_false(is_ready(when_all_future));

	//set str futures:
	promise_str_val.set_value(helpers::result_factory<std::string>::get());
	assert_false(is_ready(when_all_future));

	const size_t str_err_id = randomizer();
	promise_str_err.set_exception(make_costume_exception(str_err_id));
	assert_false(is_ready(when_all_future));

	//set void futures:
	promise_void_val.set_value();
	assert_false(is_ready(when_all_future));
	
	const size_t void_err_id = randomizer();
	promise_void_err.set_exception(make_costume_exception(void_err_id));
	assert_false(is_ready(when_all_future));

	//set int& futures
	promise_int_ref_val.set_value(helpers::result_factory<int&>::get());
	assert_false(is_ready(when_all_future));
	
	const size_t int_ref_err_id = randomizer();
	promise_int_ref_err.set_exception(make_costume_exception(int_ref_err_id));
	assert_false(is_ready(when_all_future));

	//set std::string& futures
	promise_str_ref_val.set_value(helpers::result_factory<std::string&>::get());
	assert_false(is_ready(when_all_future));
	
	const size_t str_ref_err_id = randomizer();
	promise_str_ref_err.set_exception(make_costume_exception(str_ref_err_id));

	assert_true(is_ready(when_all_future));
	auto result = when_all_future.get();
	assert_false(when_all_future.valid());

	helpers::test_ready_future_result(std::move(std::get<0>(result)));
	helpers::test_ready_future_result(std::move(std::get<2>(result)));
	helpers::test_ready_future_result(std::move(std::get<4>(result)));
	helpers::test_ready_future_result(std::move(std::get<6>(result)));
	helpers::test_ready_future_result(std::move(std::get<8>(result)));

	helpers::test_ready_future_costume_exception(std::move(std::get<1>(result)), int_err_id);
	helpers::test_ready_future_costume_exception(std::move(std::get<3>(result)), str_err_id);
	helpers::test_ready_future_costume_exception(std::move(std::get<5>(result)), void_err_id);
	helpers::test_ready_future_costume_exception(std::move(std::get<7>(result)), int_ref_err_id);
	helpers::test_ready_future_costume_exception(std::move(std::get<9>(result)), str_ref_err_id);
}

void concurrencpp::tests::test_when_all(){
	test_when_all_empty();
	test_when_all_many();
}


void concurrencpp::tests::test_when_any_empty() {
	concurrencpp::future<concurrencpp::when_any_result<std::tuple<>>> future = ::concurrencpp::when_any();
	assert_true(future.valid());
	assert_same(future.wait_for(::std::chrono::seconds(0)), std::future_status::ready);

	auto res = future.get();

	assert_same(res.index, static_cast<size_t>(-1));
}

template<size_t index>
void concurrencpp::tests::test_when_any_many(helpers::random& randomizer){
	auto is_ready = [](const auto& future) {return future.wait_for(std::chrono::seconds(0)) != std::future_status::timeout; };
	auto make_costume_exception = [](auto id) {return ::std::make_exception_ptr(helpers::costume_exception(id)); };

	::concurrencpp::promise<int> int_val_promise, int_err_promise;
	::concurrencpp::promise<std::string> str_val_promise, str_err_promise;
	::concurrencpp::promise<void> void_val_promise, void_err_promise;
	::concurrencpp::promise<int&> int_ref_val_promise, int_ref_err_promise;
	::concurrencpp::promise<std::string&> str_ref_val_promise, str_ref_err_promise;

	auto int_val_future = int_val_promise.get_future();
	auto int_err_future = int_err_promise.get_future();
	auto str_val_future = str_val_promise.get_future();
	auto str_err_future = str_err_promise.get_future();
	auto void_val_future = void_val_promise.get_future();
	auto void_err_future = void_err_promise.get_future();
	auto int_ref_val_future = int_ref_val_promise.get_future();
	auto int_ref_err_future = int_ref_err_promise.get_future();
	auto str_ref_val_future = str_ref_val_promise.get_future();
	auto str_ref_err_future = str_ref_err_promise.get_future();

	auto when_any_future = ::concurrencpp::when_any(
		int_val_future,
		int_err_future,
		str_val_future,
		str_err_future,
		void_val_future,
		void_err_future,
		int_ref_val_future,
		int_ref_err_future,
		str_ref_val_future,
		str_ref_err_future);

	auto all_promises = std::tie(
		int_val_promise,
		int_err_promise,
		str_val_promise,
		str_err_promise,
		void_val_promise,
		void_err_promise,
		int_ref_val_promise,
		int_ref_err_promise,
		str_ref_val_promise,
		str_ref_err_promise);

	assert_false(is_ready(when_any_future));

	auto& chosen_promise = std::get<index>(all_promises);
	using type = typename tests::helpers::type_from_promise<std::remove_reference_t<decltype(chosen_promise)>>::type;
	const auto exception_id = randomizer();

	if (index % 2 == 0) {
		helpers::promise_setter::execute_set_promise(chosen_promise, helpers::result_factory<type>::get);
	}
	else {
		chosen_promise.set_exception(make_costume_exception(exception_id));
	}

	assert_true(is_ready(when_any_future));

	auto when_any_result = when_any_future.get();
	assert_same(when_any_result.index, index);

	auto& futures = when_any_result.futures;
	assert_true(std::get<0>(futures).valid());
	assert_true(std::get<1>(futures).valid());
	assert_true(std::get<2>(futures).valid());
	assert_true(std::get<3>(futures).valid());
	assert_true(std::get<4>(futures).valid());
	assert_true(std::get<5>(futures).valid());
	assert_true(std::get<6>(futures).valid());
	assert_true(std::get<7>(futures).valid());
	assert_true(std::get<8>(futures).valid());
	assert_true(std::get<9>(futures).valid());

	assert_same(is_ready(std::get<0>(futures)), index == 0);
	assert_same(is_ready(std::get<1>(futures)), index == 1);
	assert_same(is_ready(std::get<2>(futures)), index == 2);
	assert_same(is_ready(std::get<3>(futures)), index == 3);
	assert_same(is_ready(std::get<4>(futures)), index == 4);
	assert_same(is_ready(std::get<5>(futures)), index == 5);
	assert_same(is_ready(std::get<6>(futures)), index == 6);
	assert_same(is_ready(std::get<7>(futures)), index == 7);
	assert_same(is_ready(std::get<8>(futures)), index == 8);
	assert_same(is_ready(std::get<9>(futures)), index == 9);

	auto ready_future = std::move(std::get<index>(when_any_result.futures));
	assert_true(is_ready(ready_future));

	if (index % 2 == 0) {
		helpers::test_ready_future_result(std::move(ready_future));
	}
	else {
		helpers::test_ready_future_costume_exception(std::move(ready_future), exception_id);
	}
}

void concurrencpp::tests::test_when_any(){
	test_when_any_empty();

	helpers::random randomizer;
	test_when_any_many<0>(randomizer);
	test_when_any_many<1>(randomizer);
	test_when_any_many<2>(randomizer);
	test_when_any_many<3>(randomizer);
	test_when_any_many<4>(randomizer);
	test_when_any_many<5>(randomizer);
	test_when_any_many<6>(randomizer);
	test_when_any_many<7>(randomizer);
	test_when_any_many<8>(randomizer);
	test_when_any_many<9>(randomizer);
}

void concurrencpp::tests::test_future() {
	test_future_valid();
	test_future_get();
	test_future_wait();
	test_future_wait_for();
	test_future_wait_until();
	test_future_then();

	test_make_ready_future();
	test_make_exceptional_future();

	test_when_all();
	test_when_any();
}