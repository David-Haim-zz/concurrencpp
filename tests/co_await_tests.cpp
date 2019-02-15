#include "concurrencpp_tests.h"
#include "../concurrencpp.h"

#include <thread>
#include <chrono>

namespace concurrencpp {
	namespace tests {

		template<class type>
		void test_co_await_result(future<type> future);

		template<class type>
		void test_co_await_invalid_future_impl();
		void test_co_await_invalid_future();

		template<class type>
		void test_co_await_ready_future_value_impl();

		template<class type>
		void test_co_await_unready_future_value_impl();
		void test_co_await_value();

		template<class type>
		void test_co_await_ready_exception_future_impl(size_t id);

		template<class type>
		void test_co_await_unready_exception_future_impl(size_t id);	
		void test_co_await_exception();
	}
}

namespace concurrencpp {
	namespace tests {
		namespace helpers {
			
			class tester {
				template<class type>
				concurrencpp::future<void> test_value(concurrencpp::future<type> future) {
					try {
						assert_same(helpers::result_factory<type>::get(), co_await future);
					}
					catch (...) {
						assert_true(false);
					}
				}

				concurrencpp::future<void> test_value(concurrencpp::future<void> future) {
					try {
						co_await future;
					}
					catch (...) {
						assert_true(false);
					}
				}

				template<class type>
				concurrencpp::future<void> test_exception(concurrencpp::future<type> future, size_t id) {
					try {
						co_await future;
					}
					catch (helpers::costume_exception exception) {
						assert_same(exception.id, id);
					}
				}

			public:
				template<class type>
				void operator()(concurrencpp::future<type> future) {
					test_value(std::move(future)).get();
				}

				template<class type>
				void operator()(concurrencpp::future<type> future, size_t id) {
					test_exception(std::move(future), id).get();
				}	
			};
		}
	}

}

template<class type>
void concurrencpp::tests::test_co_await_result(future<type> future){
	helpers::tester tester;
	tester(std::move(future));
}

template<class type>
void concurrencpp::tests::test_co_await_invalid_future_impl(){
	::concurrencpp::future<type> future;

	try {
		co_await future;
	}
	catch (std::future_error e) {
		assert_same(e.code(), std::future_errc::no_state);
		return;
	}
	catch (...) {
		//do nothing
	}

	assert_true(false);
}

void concurrencpp::tests::test_co_await_invalid_future(){
	test_co_await_invalid_future_impl<int>();
	test_co_await_invalid_future_impl<std::string>();
	test_co_await_invalid_future_impl<void>();
	test_co_await_invalid_future_impl<int&>();
	test_co_await_invalid_future_impl<std::string&>();
}

template<class type>
void concurrencpp::tests::test_co_await_ready_future_value_impl(){
	concurrencpp::promise<type> promise;
	helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);
	auto future = promise.get_future();
	test_co_await_result(std::move(future));
}

template<class type>
void concurrencpp::tests::test_co_await_unready_future_value_impl(){
	concurrencpp::promise<type> promise;
	auto future = promise.get_future();

	concurrencpp::spawn([promise = std::move(promise)]() mutable {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		helpers::promise_setter::execute_set_promise(promise, helpers::result_factory<type>::get);
	});

	test_co_await_result(std::move(future));
}

void concurrencpp::tests::test_co_await_value() {
	test_co_await_ready_future_value_impl<int>();
	test_co_await_ready_future_value_impl<std::string>();
	test_co_await_ready_future_value_impl<void>();
	test_co_await_ready_future_value_impl<int&>();
	test_co_await_ready_future_value_impl<std::string&>();

	test_co_await_unready_future_value_impl<int>();
	test_co_await_unready_future_value_impl<std::string>();
	test_co_await_unready_future_value_impl<void>();
	test_co_await_unready_future_value_impl<int&>();
	test_co_await_unready_future_value_impl<std::string&>();
}

template<class type>
void concurrencpp::tests::test_co_await_ready_exception_future_impl(size_t id){
	auto ready_future = concurrencpp::make_exceptional_future<type, helpers::costume_exception>(id);

	helpers::tester tester;
	tester(std::move(ready_future), id);
}

template<class type>
void concurrencpp::tests::test_co_await_unready_exception_future_impl(size_t id){
	concurrencpp::promise<type> promise;
	auto future = promise.get_future();
	concurrencpp::spawn([promise = std::move(promise), id]() mutable {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		promise.set_exception(std::make_exception_ptr(helpers::costume_exception(id)));
	});

	helpers::tester tester;
	tester(std::move(future), id);
}

void concurrencpp::tests::test_co_await_exception(){
	helpers::random randomizer;

	test_co_await_ready_exception_future_impl<int>(randomizer());
	test_co_await_ready_exception_future_impl<std::string>(randomizer());
	test_co_await_ready_exception_future_impl<void>(randomizer());
	test_co_await_ready_exception_future_impl<int&>(randomizer());
	test_co_await_ready_exception_future_impl<std::string&>(randomizer());

	test_co_await_unready_exception_future_impl<int>(randomizer());
	test_co_await_unready_exception_future_impl<std::string>(randomizer());
	test_co_await_unready_exception_future_impl<void>(randomizer());
	test_co_await_unready_exception_future_impl<int&>(randomizer());
	test_co_await_unready_exception_future_impl<std::string&>(randomizer());
}

void concurrencpp::tests::test_co_await(){
	test_co_await_invalid_future();
	test_co_await_value();
	test_co_await_exception();
}