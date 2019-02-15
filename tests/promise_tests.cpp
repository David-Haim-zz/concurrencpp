#include "concurrencpp_tests.h"
#include "../concurrencpp.h"

#include <string>
#include <tuple>

namespace concurrencpp {
	namespace tests {

		template<class type>
		void test_promise_valid_impl();
		void test_promise_valid();

		template<class type>
		void test_promise_get_future_impl();
		void test_promise_get_future();

		template<class type, class ... argument_types>
		void test_promise_set_value_impl(argument_types&& ... args);
		void test_promise_set_value();

		template<class type>
		void test_promise_set_exception_impl();
		void test_promise_set_exception();
	
		template<class type>
		void test_promise_assignment_operator_impl();
		void test_promise_assignment_operator();


		template <class type, class ... argument_types>
		inline auto bind_promise(concurrencpp::promise<type> promise, argument_types&& ... args) {
			return[
				p = std::move(promise),
				arg_tuple = std::tuple<argument_types...>(std::forward<argument_types>(args)...)]() mutable {
				std::apply([&](auto&& ... args) mutable { p.set_value(args...); }, arg_tuple);
			};
		}

	}
}

template<class type>
void concurrencpp::tests::test_promise_valid_impl(){
	{	// invalid promise: moved.
		::concurrencpp::promise<type> promise;
		assert_true(promise.valid());
		auto dummy = std::move(promise);
		assert_false(promise.valid());
	}

	{	//set value - promise still valid
		::concurrencpp::promise<type> promise;
		helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);
		assert_true(promise.valid());
	}

	{	//set exception
		::concurrencpp::promise<type> promise;
		promise.set_exception(std::make_exception_ptr(std::exception()));
		assert_true(promise.valid());
	}
}

void concurrencpp::tests::test_promise_valid(){
	test_promise_valid_impl<int>();
	test_promise_valid_impl<std::string>();
	test_promise_valid_impl<void>();
	test_promise_valid_impl<int&>();
	test_promise_valid_impl<std::string&>();
}

template<class type>
void concurrencpp::tests::test_promise_get_future_impl(){
	//trying to get the future more than once throws
	::concurrencpp::promise<type> promise;
	auto future = promise.get_future();

	assert_true(future.valid());
	assert_same(future.wait_for(std::chrono::seconds(0)), std::future_status::timeout);

	assert_throws<std::future_error>(
		[&] { promise.get_future(); },
		[](std::future_error e) { assert_same(e.code(), std::future_errc::future_already_retrieved); });

	//empty promise should throw
	auto dummy = std::move(promise);
	assert_throws<std::future_error>(
		[&] { promise.get_future(); },
		[](std::future_error e) { assert_same(e.code(), std::future_errc::no_state); });
}

void concurrencpp::tests::test_promise_get_future(){
	test_promise_get_future_impl<int>();
	test_promise_get_future_impl<std::string>();
	test_promise_get_future_impl<void>();
	test_promise_get_future_impl<int&>();
	test_promise_get_future_impl<std::string&>();
}

template<class type, class ... argument_types>
void concurrencpp::tests::test_promise_set_value_impl(argument_types&& ... args) {	
	{	//basic test:
		concurrencpp::promise<type> promise;
		auto future = promise.get_future();	

		std::thread thread(bind_promise(std::move(promise), std::forward<argument_types>(args)...));
		thread.join();

		helpers::test_ready_future_result(std::move(future));
	}

	{	//it's ok to set a value on a promise that hasn't provided a future yet.
		concurrencpp::promise<type> promise;
		promise.set_value(std::forward<argument_types>(args)...);
		auto future = promise.get_future();
		helpers::test_ready_future_result(std::move(future));
	}

	//if exception is thrown inside set_value (by building an object in place) the future is un-ready
	{
		concurrencpp::promise<helpers::object_throws_on_construction> promise;
		auto future = promise.get_future();
		
		assert_throws<std::exception>([&] {
			promise.set_value(1, 2);
		});
		assert_true(promise.valid());
		assert_same(::std::future_status::timeout, future.wait_for(std::chrono::seconds(0)));
	}

	{	//can't set result to an empty promise
		concurrencpp::promise<type> promise;
		auto dummy = std::move(promise);

		assert_throws<std::future_error>(
			bind_promise(std::move(promise), std::forward<argument_types>(args)...),
			[](std::future_error e) { assert_same(e.code(), std::future_errc::no_state); });
	}

	{	//can't set result twice
		concurrencpp::promise<type> promise;
		promise.set_value(std::forward<argument_types>(args)...);

		assert_throws<std::future_error>(
			bind_promise(std::move(promise), std::forward<argument_types>(args)...),
			[](std::future_error e) { assert_same(e.code(), std::future_errc::promise_already_satisfied); });
	}

	//can't set result if exception was set.
	{
		concurrencpp::promise<type> promise;
		promise.set_exception(std::make_exception_ptr(std::exception()));
	
		assert_throws<std::future_error>(
			bind_promise(std::move(promise), std::forward<argument_types>(args)...),
			[](std::future_error e) {assert_same(e.code(), std::future_errc::promise_already_satisfied); });
	}
}

void concurrencpp::tests::test_promise_set_value() {
	const auto test_string = "   " + helpers::result_factory<std::string>::get() + "   ";

	test_promise_set_value_impl<int>(helpers::result_factory<int>::get());
	test_promise_set_value_impl<std::string>(test_string, 3, test_string.length() - 6);
	test_promise_set_value_impl<void>();
	test_promise_set_value_impl<int&>(helpers::result_factory<int&>::get());
	test_promise_set_value_impl<std::string&>(helpers::result_factory<std::string&>::get());
}

template<class type>
void concurrencpp::tests::test_promise_set_exception_impl(){
	helpers::random randomizer;

	{	//basic test:
		concurrencpp::promise<type> promise;
		auto future = promise.get_future();
		const auto id = randomizer();

		std::thread thread([promise = std::move(promise), id]() mutable {
			promise.set_exception(std::make_exception_ptr<helpers::costume_exception>(id));
		});
		thread.join();

		helpers::test_ready_future_costume_exception(std::move(future), id);
	}

	{	//it's ok to set a value on a promise that hasn't provided a future yet.
		concurrencpp::promise<type> promise;
		const auto id = randomizer();

		promise.set_exception(std::make_exception_ptr<helpers::costume_exception>(id));
		auto future = promise.get_future();
		helpers::test_ready_future_costume_exception(std::move(future), id);
	}

	{	//if exception is thrown inside set_exception (by building an exception in place) the future is un-ready
		concurrencpp::promise<type> promise;
		auto future = promise.get_future();

		assert_throws<std::exception>([&] {
			promise.set_exception(std::make_exception_ptr(helpers::exception_throws_on_construction()));
		});

		assert_true(promise.valid());
		assert_same(::std::future_status::timeout, future.wait_for(std::chrono::seconds(0)));
	}

	{	//can't set result to an empty promise
		concurrencpp::promise<type> promise;
		auto dummy = std::move(promise);

		assert_throws<std::future_error>(
			[&promise] { promise.set_exception(std::make_exception_ptr(std::exception())); },
			[](std::future_error e) { assert_same(e.code(), std::future_errc::no_state); });
	}

	{	//can't set exception twice
		concurrencpp::promise<type> promise;
		promise.set_exception(std::make_exception_ptr(std::exception()));

		assert_throws<std::future_error>(
			[&promise] { promise.set_exception(std::make_exception_ptr(std::exception())); },
			[](std::future_error e) { assert_same(e.code(), std::future_errc::promise_already_satisfied); });
	}

	//can't set result if value was set.
	{
		concurrencpp::promise<type> promise;
		helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);

		assert_throws<std::future_error>(
			[&promise] { promise.set_exception(std::make_exception_ptr(std::exception())); },
			[](std::future_error e) {assert_same(e.code(), std::future_errc::promise_already_satisfied); });
	}
}

void concurrencpp::tests::test_promise_set_exception() {
	test_promise_set_exception_impl<int>();
	test_promise_set_exception_impl<std::string>();
	test_promise_set_exception_impl<void>();
	test_promise_set_exception_impl<int&>();
	test_promise_set_exception_impl<std::string&>();
}

template<class type>
void concurrencpp::tests::test_promise_assignment_operator_impl(){
	{	//non empty -> non empty, breaks the promise
		concurrencpp::promise<type> promise_a, promise_b;
		auto future_a = promise_a.get_future();
		auto future_b = promise_b.get_future();

		promise_a = std::move(promise_b);

		assert_true(promise_a.valid());
		assert_false(promise_b.valid());

		assert_throws<std::future_error>([&] {
			future_a.get();
		},  
		[](auto e) {
			assert_same(e.code(), std::future_errc::broken_promise);
		});

		tests::helpers::promise_setter::execute_set_promise(promise_a, helpers::result_factory<type>::get);
		helpers::test_ready_future_result(std::move(future_b));
	}
	
	//empty -> non empty. breaks the promise
	{
		concurrencpp::promise<type> promise_a, promise_b, dummy;
		auto future_a = promise_a.get_future();
		auto future_b = promise_b.get_future();

		dummy = std::move(promise_b); //promise_b now empty
		promise_a = std::move(promise_b);
		assert_throws<std::future_error>([&] {
			future_a.get();
		}, [](auto e) {
			assert_same(e.code(), std::future_errc::broken_promise);
		});
	}

	//non empty -> empty
	{
		concurrencpp::promise<type> promise_a, promise_b, dummy;
		auto future_a = promise_a.get_future();
		auto future_b = promise_b.get_future();

		dummy = std::move(promise_a); //promise_a now empty
		promise_a = std::move(promise_b);
	
		tests::helpers::promise_setter::execute_set_promise(promise_a, helpers::result_factory<type>::get);
		helpers::test_ready_future_result(std::move(future_b));
	}
	
	//empty -> empty
	{
		concurrencpp::promise<type> promise_a, promise_b, dummy;
		auto future_a = promise_a.get_future();
		auto future_b = promise_b.get_future();

		dummy = std::move(promise_a); //promise_a now empty
		dummy = std::move(promise_b); //promise_b is now empty

		promise_a = std::move(promise_b);
		assert_false(promise_a.valid());
		assert_false(promise_b.valid());
	}
}

void concurrencpp::tests::test_promise_assignment_operator() {
	test_promise_assignment_operator_impl<int>();
	test_promise_assignment_operator_impl<std::string>();
	test_promise_assignment_operator_impl<void>();
	test_promise_assignment_operator_impl<int&>();
	test_promise_assignment_operator_impl<std::string&>();
}

void concurrencpp::tests::test_promise(){
	test_promise_valid();
	test_promise_get_future();
	test_promise_set_value();
	test_promise_set_exception();
	test_promise_assignment_operator();
}