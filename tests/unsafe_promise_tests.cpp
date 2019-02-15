#include "concurrencpp_tests.h"
#include "../concurrencpp.h"

namespace concurrencpp {
	namespace tests {
	
		template<class type, class ... argument_types>
		void test_unsafe_promise_set_value_impl(argument_types&& ... args);
		void test_unsafe_promise_set_value();

		template<class type>
		void test_unsafe_promise_set_exception_impl(helpers::random& randomizer);
		void test_unsafe_promise_set_exception();

		template<class function_type>
		void test_unsafe_promise_set_from_function_val_impl(function_type&& function);
		template<class type>
		void test_unsafe_promise_set_from_function_err_impl(helpers::random& randomizer);
		void test_unsafe_promise_set_from_function();

		template<class type>
		void test_unsafe_promise_set_from_future_val();
		template<class type>
		void test_unsafe_promise_set_from_future_err(helpers::random& randomizer);
		void test_unsafe_promise_set_from_future();
	}
}

template<class type, class ...argument_types>
void concurrencpp::tests::test_unsafe_promise_set_value_impl(argument_types && ...args){
	//sync test
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();
		promise.set_value(std::forward<argument_types>(args)...);
		helpers::test_ready_future_result(std::move(future));
	}

	//async test
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();

		std::thread thread([future = std::move(future)]() mutable {
			future.wait();
			helpers::test_ready_future_result(std::move(future));
		});

		std::this_thread::yield();
		promise.set_value(std::forward<argument_types>(args)...);
		thread.join();
	}

	//if future holds "then", set_value calls it.
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto ok = false;
		auto future = promise.get_future();
		auto id = std::thread::id();

		future.then([&ok, &id](auto done_future) {	
			ok = true;
			id = std::this_thread::get_id();
			helpers::test_ready_future_result(std::move(done_future));
		});

		promise.set_value(std::forward<argument_types>(args)...);
		assert_true(ok);
		assert_same(id, std::this_thread::get_id());
	}
}

void concurrencpp::tests::test_unsafe_promise_set_value() {
	const auto test_string = "   " + helpers::result_factory<std::string>::get() + "   ";

	test_unsafe_promise_set_value_impl<int>(helpers::result_factory<int>::get());
	test_unsafe_promise_set_value_impl<std::string>(test_string, 3, test_string.length() - 6);
	test_unsafe_promise_set_value_impl<void>();
	test_unsafe_promise_set_value_impl<int&>(helpers::result_factory<int&>::get());
	test_unsafe_promise_set_value_impl<std::string&>(helpers::result_factory<std::string&>::get());
}

template<class type>
void concurrencpp::tests::test_unsafe_promise_set_exception_impl(helpers::random& randomizer) {
	//sync version
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();
		const auto id = randomizer();
		promise.set_exception(std::make_exception_ptr<helpers::costume_exception>(id));
		helpers::test_ready_future_costume_exception(std::move(future), id);
	}

	//async version
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();
		const auto id = randomizer();

		std::thread thread([future = std::move(future), id]() mutable {
			future.wait();
			helpers::test_ready_future_costume_exception(std::move(future), id);
		});

		promise.set_exception(std::make_exception_ptr<helpers::costume_exception>(id));
		thread.join();
	}

	//then: if future holds "then", set_exception calls it.
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();

		auto ok = false;
		auto thread_id = std::thread::id();
		const auto id = randomizer();

		future.then([&ok, &thread_id, id](auto done_future) {
			ok = true;
			thread_id = std::this_thread::get_id();
			helpers::test_ready_future_costume_exception(std::move(done_future), id);
		});

		promise.set_exception(std::make_exception_ptr<helpers::costume_exception>(id));
		assert_true(ok);
		assert_same(thread_id, std::this_thread::get_id());
	}
}

void concurrencpp::tests::test_unsafe_promise_set_exception() {
	helpers::random randomizer;

	test_unsafe_promise_set_exception_impl<int>(randomizer);
	test_unsafe_promise_set_exception_impl<std::string>(randomizer);
	test_unsafe_promise_set_exception_impl<void>(randomizer);
	test_unsafe_promise_set_exception_impl<int&>(randomizer);
	test_unsafe_promise_set_exception_impl<std::string&>(randomizer);
}

template<class function_type>
void concurrencpp::tests::test_unsafe_promise_set_from_function_val_impl(function_type && function){
	using type = std::result_of_t<function_type()>;

	//sync test
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();
		promise.set_from_function(std::forward<function_type>(function));
		helpers::test_ready_future_result(std::move(future));
	}

	//async test
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();
		std::thread thread([future = std::move(future)]() mutable {
			future.wait();
			helpers::test_ready_future_result(std::move(future));
		});
		promise.set_from_function(std::forward<function_type>(function));
		thread.join();
	}

	//if future holds "then", set_from_function calls it.
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto ok = false;
		auto future = promise.get_future();
		auto thread_id = std::thread::id();

		future.then([&ok, &thread_id](auto done_future) {
			ok = true;
			thread_id = std::this_thread::get_id();
			helpers::test_ready_future_result(std::move(done_future));
		});

		promise.set_from_function(std::forward<function_type>(function));
		assert_true(ok);
		assert_same(thread_id, std::this_thread::get_id());
	}
}

template<class type>
void concurrencpp::tests::test_unsafe_promise_set_from_function_err_impl(helpers::random& randomizer){
	//sync version
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();
		const auto id = randomizer();

		promise.set_from_function([id] ()-> decltype(auto) {
			throw helpers::costume_exception(id);
			return helpers::result_factory<type>::get();
		});

		helpers::test_ready_future_costume_exception(std::move(future), id);
	}

	//async version
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();
		const auto id = randomizer();

		std::thread thread([id, future = std::move(future)] () mutable {
			future.wait();
			helpers::test_ready_future_costume_exception(std::move(future), id);
		});

		promise.set_from_function([id] () -> decltype(auto){
			throw helpers::costume_exception(id);
			return helpers::result_factory<type>::get();
		});

		thread.join();
	}

	//if future holds "then", set_from_function calls it.
	{
		concurrencpp::details::unsafe_promise<type> promise;
		auto future = promise.get_future();

		auto ok = false;
		auto thread_id = std::thread::id();
		const auto id = randomizer();
	
		future.then([&ok, &thread_id](auto done_future) {
			ok = true;
			thread_id = std::this_thread::get_id();
			helpers::test_ready_future_result(std::move(done_future));
		});

		promise.set_from_function([id]()->decltype(auto) {
			throw helpers::costume_exception(id);
			return helpers::result_factory<type>::get();
		});

		assert_true(ok);
		assert_same(thread_id, std::this_thread::get_id());
	}
}

void concurrencpp::tests::test_unsafe_promise_set_from_function(){
	test_unsafe_promise_set_from_function_val_impl(helpers::result_factory<int>::get);
	test_unsafe_promise_set_from_function_val_impl(helpers::result_factory<std::string>::get);
	test_unsafe_promise_set_from_function_val_impl(helpers::result_factory<void>::get);
	test_unsafe_promise_set_from_function_val_impl(helpers::result_factory<int&>::get);
	test_unsafe_promise_set_from_function_val_impl(helpers::result_factory<std::string&>::get);

	helpers::random randomizer;
	test_unsafe_promise_set_from_function_err_impl<int>(randomizer);
	test_unsafe_promise_set_from_function_err_impl<std::string>(randomizer);
	test_unsafe_promise_set_from_function_err_impl<void>(randomizer);
	test_unsafe_promise_set_from_function_err_impl<int&>(randomizer);
	test_unsafe_promise_set_from_function_err_impl<std::string&>(randomizer);
}

template<class type>
void concurrencpp::tests::test_unsafe_promise_set_from_future_val(){
	details::unsafe_promise<type> promise;
	promise.set_from_function(helpers::result_factory<type>::get);
	auto done_future = promise.get_future();

	concurrencpp::details::unsafe_promise<type> test_promise;
	auto test_future = test_promise.get_future();
	test_promise.set_from_future(std::move(done_future));
	helpers::test_ready_future_result(std::move(test_future));
}

template<class type>
void concurrencpp::tests::test_unsafe_promise_set_from_future_err(helpers::random& randomizer){
	const auto id = randomizer();
	auto done_future = concurrencpp::make_exceptional_future<type,helpers::costume_exception>(id);
	details::unsafe_promise<type> promise;
	promise.set_from_future(done_future);
	helpers::test_ready_future_costume_exception(promise.get_future(), id);
}

void concurrencpp::tests::test_unsafe_promise_set_from_future(){
	test_unsafe_promise_set_from_future_val<int>();
	test_unsafe_promise_set_from_future_val<std::string>();
	test_unsafe_promise_set_from_future_val<void>();
	test_unsafe_promise_set_from_future_val<int&>();
	test_unsafe_promise_set_from_future_val<std::string&>();

	helpers::random randomizer;
	test_unsafe_promise_set_from_future_err<int>(randomizer);
	test_unsafe_promise_set_from_future_err<std::string>(randomizer);
	test_unsafe_promise_set_from_future_err<void>(randomizer);
	test_unsafe_promise_set_from_future_err<int&>(randomizer);
	test_unsafe_promise_set_from_future_err<std::string&>(randomizer);
}

void concurrencpp::tests::test_unsafe_promise(){
	test_unsafe_promise_set_value();
	test_unsafe_promise_set_exception();
	test_unsafe_promise_set_from_function();
	test_unsafe_promise_set_from_future();
}