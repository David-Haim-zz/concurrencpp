#ifndef CONCURRENCPP_TESTS_H
#define CONCURRENCPP_TESTS_H

#include "..\concurrencpp.h"

#include <atomic>

namespace concurrencpp {
	namespace tests {
		void assert_true(bool condition);
		void assert_false(bool condition);

		template<class a_type , class b_type>
		void assert_same(a_type&& a, b_type&& b) {
			assert_true(a == b);
		}

		template<class a_type, class b_type>
		void assert_not_same(a_type&& a, b_type&& b) {
			assert_true(a != b);
		}

		template<class exception_type, class task_type, class comparator_type>
		void assert_throws(task_type&& task, comparator_type&& comparator) {
			try {
				task();
				assert_false(true);
			}
			catch (const exception_type& e) {
				comparator(e);
			}
			catch (...) {
				assert_false(true);
			}
		}

		template<class exception_type, class task_type>
		void assert_throws(task_type&& task) {
			assert_throws<exception_type>(std::forward<task_type>(task), [](auto e) {});
		}
	}	
}

namespace concurrencpp {
	namespace tests {
		void test_spin_lock();
		void test_recursive_spin_lock();
		void test_allocate_deallocate();
		void test_thread_pool();
		void test_unsafe_promise();
		void test_promise();
		void test_future();
		void test_co_await();
		void test_spawn_async();
		void test_timer();
		void test_all();
	}
}

namespace concurrencpp {
	namespace tests {
		namespace helpers {
			class random {

			private:
				int64_t m_state;

				inline static int64_t xor_shift_64(int64_t i) noexcept {
					i ^= (i << 21);
					i ^= (i >> 35);
					i ^= (i << 4);
					return i;
				}

			public:	
				random() : m_state(static_cast<int64_t>(details::time_from_epoch() | 0xCAFEBABE)) {}

				int64_t operator() () {
					auto res = m_state;
					m_state = xor_shift_64(m_state);
					return res;
				}

				int64_t operator()(int64_t min, int64_t max) {
					auto r = (*this)();
					auto upper_limit = max - min + 1;
					return std::abs(r) % upper_limit + min;
				}
			};
		}
	}
}

namespace concurrencpp {
	namespace tests {
		namespace helpers {	
			class raii_tester { //used to check if an object is destructed correctly.
			private:
				std::shared_ptr<::std::atomic_size_t> m_counter;

				raii_tester(const raii_tester&) noexcept = default;

			public:
				raii_tester() : m_counter(std::make_shared<std::atomic_size_t>(0)) {}
				raii_tester(raii_tester&&) noexcept = default;
				
				~raii_tester() noexcept {
					if (static_cast<bool>(m_counter)) {
						++(*m_counter);
					}
				}

				inline void operator() () const noexcept {}

				raii_tester clone_context() const noexcept{ return *this; }

				void test(size_t expected) noexcept {
					assert(static_cast<bool>(m_counter));
					assert_same(m_counter->load(), expected);
				}
			};
		}
	}
}
namespace concurrencpp {
	namespace tests {
		namespace helpers {			
			//various classes for costume tests like in-place construction, binding etc.
			struct object_throws_on_construction {
				object_throws_on_construction(int, int) { throw std::exception(); }
			};

			struct exception_throws_on_construction : public std::runtime_error {
				exception_throws_on_construction() : runtime_error("") { throw std::exception(); }
			};

			class delegate_no_args {

			private:
				std::atomic_size_t* m_counter;

			public:
				inline delegate_no_args(std::atomic_size_t& counter) noexcept : m_counter(&counter) {}
				inline delegate_no_args(delegate_no_args&& rhs) noexcept : m_counter(rhs.m_counter) { rhs.m_counter = nullptr; }
				inline void operator() () noexcept { if (m_counter != nullptr) { ++(*m_counter); } }
			};

			struct delegate_args {
				inline void operator() (std::atomic_size_t& counter) const noexcept { ++counter; }
			};

			struct aborts_on_copy_or_move {
				aborts_on_copy_or_move(const aborts_on_copy_or_move&) noexcept { assert_true(false); }
				aborts_on_copy_or_move(aborts_on_copy_or_move&&) noexcept { assert_true(false); }

				aborts_on_copy_or_move(int, int) noexcept {}
			};
		}
	}
}

namespace concurrencpp {
	namespace tests {
		namespace helpers {
			struct costume_exception : public std::exception {
				const size_t id;

				costume_exception(size_t id) noexcept : id(id) {}
			};

			template<class type>
			void test_ready_future_costume_exception(future<type> future, const size_t id) {
				assert_true(future.valid());
				assert_same(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);

				try {
					future.get();
				}
				catch (costume_exception e) {
					return assert_same(e.id, id);
				}
				catch (...) {}

				assert_true(false);
			}

		}
	}
}

namespace concurrencpp {
	namespace tests {
		namespace helpers {
			class waiting_thread {
				//A thread that is being launched, then waits for some callable to execute.

			private:
				std::unique_ptr<::concurrencpp::details::callback_base> m_function;
				std::mutex m_mutex;
				std::condition_variable m_variable;
				std::thread m_thread;

				void thread_func() noexcept;

			public:
				waiting_thread() { m_thread = std::thread(&waiting_thread::thread_func, this); }
				~waiting_thread();

				template<class function_type>
				void set_function(function_type&& function) {
					std::unique_lock<std::mutex> lock(m_mutex);
					m_function = ::concurrencpp::details::make_callback(std::forward<function_type>(function));
				}

				inline void join() { m_thread.join(); }
				inline void resume() { m_variable.notify_one(); }
				inline std::thread::id get_id() const { return m_thread.get_id(); }
			};
		}
	}
}

namespace concurrencpp {
	namespace tests {
		namespace helpers {	
			template<class type>
			struct result_factory { static type get() { return type(); } };

			template<>
			struct result_factory<int> { static int get() { return 123456789; } };

			template<>
			struct result_factory<std::string> { static std::string get() { return "abcdefghijklmnopqrstuvwxyz123456789!@#$%^&*()"; } };

			template<>
			struct result_factory<void> { static void get() {} };

			template<>
			struct result_factory<int&> { static int& get() { static int i = 0; return i; } };

			template<>
			struct result_factory<std::string&> { static std::string& get() { static std::string str; return str; } };

			template<class type>
			struct val_comparator {
				bool operator()(const type& a, const type& b) { return a == b; }
			};

			template<class type>
			struct ref_comparator {
				bool operator()(const type& a, const type& b) { return std::addressof(a) == std::addressof(b); }
			};

			template<class type>
			void test_ready_future_result(future<type> future) {
				assert_true(future.valid());
				assert_same(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);

				if constexpr(std::is_reference_v<type>) {
					assert_same(&future.get(), &helpers::result_factory<type>::get());
				}
				else if constexpr(!std::is_void_v<type>) {
					assert_same(future.get(), helpers::result_factory<type>::get());
				}
				else {
					future.get(); //just make sure no exception is thrown.
				}
			}
		}
	}
}

namespace concurrencpp {
	namespace tests {
		namespace helpers {		
			//concurrencpp::promise related stuff.
			
			struct promise_setter {
				template<class type, class function_type>
				static void execute_impl(promise<type>& promise, function_type&& function) {
					promise.set_value(function());
				}

				template<class function_type>
				static void execute_impl(promise<void>& promise, function_type&& function) {
					function();
					promise.set_value();
				}

				template<class type, class function_type>
				static void execute_set_promise(promise<type>& promise, function_type&& function) noexcept {
					try {
						execute_impl(promise, function);
					}
					catch (...) {
						promise.set_exception(std::current_exception());
					}
				}
			};

			template<class type>
			struct type_from_promise {};

			template<class underlying_type>
			struct type_from_promise<::concurrencpp::promise<underlying_type>> {
				using type = underlying_type;
			};

		}
	}
}

#endif