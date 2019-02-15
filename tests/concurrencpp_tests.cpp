#include "concurrencpp_tests.h"

#include <cstdio>
#include <cstdlib>
#include <random>

void concurrencpp::tests::assert_true(bool condition){
	if (!condition) {
		fprintf(stderr, "assertion failed.\n");
		std::abort();
	}
}

void concurrencpp::tests::assert_false(bool condition){
	assert_true(!condition);
}

void concurrencpp::tests::test_all(){
	test_allocate_deallocate();
	test_spin_lock();
	test_recursive_spin_lock();
	test_thread_pool();
	test_unsafe_promise();
	test_promise();
	test_future();
	test_co_await();
	test_spawn_async();
	test_timer();
}

void concurrencpp::tests::helpers::waiting_thread::thread_func() noexcept {
	std::unique_lock<std::mutex> lock(m_mutex);
	m_variable.wait(lock, [this] {
		//returns false if the waiting should continue
		return static_cast<bool>(m_function);
	});

	assert(static_cast<bool>(m_function));
	m_function->execute();
}

concurrencpp::tests::helpers::waiting_thread::~waiting_thread() { 
	if (m_thread.joinable()) { 
		m_thread.join(); 
	} 
}
