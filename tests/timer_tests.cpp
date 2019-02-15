#include "concurrencpp_tests.h"
#include "..\concurrencpp.h"

#include <algorithm>
#include <random>

namespace concurrencpp {
	namespace tests {
		void warm_up_environment();

		template<class function_type>
		void test_empty_timer_throws(function_type&& function);

		void test_timer_constructor();
		void test_many_timers();

		void test_timer_destructor_stop_beating();
		void test_timer_destructor_raii();
		void test_timer_destructor();

		void test_timer_cancel_before_due_time();
		void test_timer_cancel_after_due_time_before_beat();
		void test_timer_cancel_after_due_time_after_beat();
		void test_timer_cancel_raii();
		void test_timer_cancel();

		void test_timer_valid();
		void test_timer_set_frequency();
		void test_timer_once();

		::concurrencpp::future<void> test_timer_delay_impl();
		void test_timer_delay();
		
		void test_timer_assignment_operator_empty_to_empty();
		void test_timer_assignment_operator_non_empty_to_non_empty();
		void test_timer_assignment_operator_empty_to_non_empty();
		void test_timer_assignment_operator_non_empty_to_empty();
		void test_timer_assignment_operator_assign_to_self();
		void test_timer_assignment_operator();
	}
}

namespace concurrencpp {
	namespace tests {
		class stop_watch {
			struct stop_watch_state {
				std::mutex lock;
				std::vector<size_t> time_points;
			};

		private:
			std::shared_ptr<stop_watch_state> m_state;

			void interval_ok(size_t interval, size_t expected);

		public:
			stop_watch();
			void operator() ();
			void test_times(size_t due_time, size_t frequency);
		};
	}
}

concurrencpp::tests::stop_watch::stop_watch(){
	m_state = std::make_shared<stop_watch_state>();
	m_state->time_points.reserve(128);
}

void concurrencpp::tests::stop_watch::interval_ok(size_t interval, size_t expected){
	const auto ratio = float(interval) / float(expected);
	assert_true(ratio >= 0.95f && ratio <= 1.15f);
}

void concurrencpp::tests::stop_watch::operator()(){
	std::unique_lock<std::mutex> lock(m_state->lock);
	m_state->time_points.emplace_back(concurrencpp::details::time_from_epoch());
}

void concurrencpp::tests::stop_watch::test_times(size_t due_time, size_t frequency){
	std::unique_lock<std::mutex> lock(m_state->lock);
	
	auto& time_points = m_state->time_points;

	assert_true(time_points.size() > 2);

	const auto test_due_time = time_points[1] - time_points[0];
	interval_ok(test_due_time, due_time);

	std::vector<size_t> frequencies;
	for (size_t i = 2; i < time_points.size(); i++) {
		frequencies.emplace_back(time_points[i] - time_points[i - 1]);
	}

	for (auto test_frequency : frequencies) {
		interval_ok(test_frequency, frequency);
	}
}

template<class function_type>
void concurrencpp::tests::test_empty_timer_throws(function_type && function){
	concurrencpp::timer moved_timer(1'000, 1'000, [] {});
	auto t = std::move(moved_timer);

	auto task = [&] { function(moved_timer); };
	assert_throws<concurrencpp::empty_timer>(task);
}

void concurrencpp::tests::warm_up_environment(){
	concurrencpp::timer dummy_timer(100, 100, [] {});
	std::this_thread::sleep_for(std::chrono::seconds(5));
}

void concurrencpp::tests::test_timer_constructor(){
	stop_watch tester;
	tester();
	timer timer(1'234, 3'457, tester);

	std::this_thread::sleep_for(std::chrono::seconds(30));
	timer.cancel();

	tester.test_times(1'234, 3'457);
}

void concurrencpp::tests::test_many_timers() {
	std::vector<timer> timers;
	std::vector<stop_watch> stop_watches;
	std::vector<std::pair<size_t, size_t>> stats;
	timers.reserve(1'000);
	stop_watches.reserve(1'000);
	stats.reserve(1'000);

	const size_t due_time_min = 1'000;
	const size_t due_time_max = 5'000;
	const size_t frequency_min = 500;
	const size_t frequency_max = 5'000;

	std::random_device rd;
	std::mt19937 mt(rd());
	std::uniform_int_distribution<int>  due_time_dist(due_time_min, due_time_max);
	std::uniform_int_distribution<int>  frequency_time_dist(frequency_min, frequency_max);

	for (size_t i = 0; i < 1'000; i++) {
		const auto due_time = due_time_dist(mt);
		const auto frequency = frequency_time_dist(mt);

		stats.emplace_back(due_time, frequency);
		auto& stop_watch = stop_watches.emplace_back();
		stop_watch();
		timers.emplace_back(due_time, frequency, stop_watch);
	}

	std::this_thread::sleep_for(std::chrono::minutes(4));

	for (auto& timer : timers) {
		timer.cancel();
	}

	std::this_thread::sleep_for(std::chrono::seconds(10));

	for (size_t i = 0; i < 1'000; i++) {
		auto pair = stats[i];
		stop_watches[i].test_times(pair.first, pair.second);
	}
}

void concurrencpp::tests::test_timer_destructor_stop_beating(){
	std::atomic_size_t counter = 0;
	size_t last_count;
	
	{
		concurrencpp::timer timer_1(100, 500, [&] {++counter; });
		auto timer_2 = std::move(timer_1); //see that nothing strange happens to an empty timer upon destruction

		std::this_thread::sleep_for(std::chrono::seconds(5));
		last_count = counter;
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	assert_same(counter, last_count);
}

void concurrencpp::tests::test_timer_destructor_raii(){
	std::vector<concurrencpp::timer> timers;
	timers.reserve(1024);
	helpers::raii_tester tester;

	for (size_t i = 0; i < 1024; i++) {
		timers.emplace_back(200 + i * 5, 500 + i * 5, tester.clone_context());
	}

	std::this_thread::sleep_for(std::chrono::seconds(10));

	std::default_random_engine rng;
	std::shuffle(std::begin(timers), std::end(timers), rng);

	size_t counter = 0;
	for (auto& timer : timers) {
		tester.test(counter);

		{
			auto timer_ = std::move(timer);
		}
		
		++counter;
		assert_false(timer.valid());
		tester.test(counter);
	}
}

void concurrencpp::tests::test_timer_destructor(){
	test_timer_destructor_stop_beating();
	test_timer_destructor_raii();
}

void concurrencpp::tests::test_timer_cancel_before_due_time(){
	std::atomic_size_t counter = 0;
	concurrencpp::timer timer(1'000, 1'000, [&] { ++counter; });
	timer.cancel();

	std::this_thread::sleep_for(std::chrono::seconds(5));

	assert_same(counter, 0);
	assert_false(timer.valid());
}

void concurrencpp::tests::test_timer_cancel_after_due_time_before_beat(){
	std::atomic_size_t counter = 0;
	concurrencpp::timer timer(500, 1'000, [&] { ++counter; });

	std::this_thread::sleep_for(std::chrono::milliseconds(600));

	timer.cancel();

	std::this_thread::sleep_for(std::chrono::seconds(5));

	assert_same(counter, 1);
	assert_false(timer.valid());
}

void concurrencpp::tests::test_timer_cancel_after_due_time_after_beat(){
	std::atomic_size_t counter = 0;
	concurrencpp::timer timer(500, 500, [&] { ++counter; });

	std::this_thread::sleep_for(std::chrono::milliseconds(1'100));

	timer.cancel();

	std::this_thread::sleep_for(std::chrono::seconds(5));

	assert_same(counter, 2);
	assert_false(timer.valid());
}

void concurrencpp::tests::test_timer_cancel_raii(){
	std::vector<concurrencpp::timer> timers;
	timers.reserve(1024);
	helpers::raii_tester tester;

	for (size_t i = 0; i < 1024; i++) {
		timers.emplace_back(200 + i * 5, 500 + i * 5, tester.clone_context());
	}

	std::this_thread::sleep_for(std::chrono::seconds(10));

	std::default_random_engine rng;
	std::shuffle(std::begin(timers), std::end(timers), rng);

	size_t counter = 0;
	for (auto& timer : timers) {
		tester.test(counter);
		timer.cancel();

		++counter;
		assert_false(timer.valid());
		tester.test(counter);
	}
}

void concurrencpp::tests::test_timer_cancel(){
	test_empty_timer_throws([](auto& timer) { timer.cancel(); });

	test_timer_cancel_before_due_time();
	test_timer_cancel_after_due_time_before_beat();
	test_timer_cancel_after_due_time_after_beat();
	test_timer_cancel_raii();
}

void concurrencpp::tests::test_timer_valid() {
	concurrencpp::timer timer_1(1'000, 1'000, [] {});
	assert_true(timer_1.valid());

	auto timer_2 = std::move(timer_1);
	assert_false(timer_1.valid());
	assert_true(timer_2.valid());

	timer_2.cancel();
	assert_false(timer_2.valid());
}

void concurrencpp::tests::test_timer_set_frequency(){
	//empty
	test_empty_timer_throws([](auto& timer) { timer.set_frequency(500); });

	//before due time
	{
		std::atomic_size_t counter = 0;
		concurrencpp::timer timer(1'000, 1'000, [&] { ++counter; });
		timer.set_frequency(500);

		std::this_thread::sleep_for(std::chrono::seconds(4));

		//timer should have launched the callback 7 timer - 1'000 + 5'00 * 6 = 4'000
		timer.cancel();

		assert_same(counter, 6);
	}

	//after due time
	{
		std::atomic_size_t counter = 0;
		concurrencpp::timer timer(2'000, 4'000, [&] { ++counter; });

		std::this_thread::sleep_for(std::chrono::milliseconds(3'000)); //2'000 + 1'000 fired once
		assert_same(counter, 1);

		timer.set_frequency(5'00);

		std::this_thread::sleep_for(std::chrono::milliseconds(3'100)); //3'000 + 100 fired once
		assert_same(counter, 2);

		std::this_thread::sleep_for(std::chrono::milliseconds(2'000)); //500 + 500 + 500 + 400 + 100 fired 4 times 
		timer.cancel(); 
	
		assert_same(counter, 6);
	}
}

void concurrencpp::tests::test_timer_once(){
	std::atomic_size_t counter = 0;
	timer::once(4'000, [&]{ ++counter; });

	for (size_t i = 0; i < 10; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(380));
		assert_same(counter, 0);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	assert_same(counter, 1);

	std::this_thread::sleep_for(std::chrono::seconds(10));
	assert_same(counter, 1);
}

::concurrencpp::future<void> concurrencpp::tests::test_timer_delay_impl() {
	stop_watch tester;
	tester();
	
	for (size_t i = 0; i < 10; i++) {
		co_await timer::delay(500);
		tester();
	}

	tester.test_times(500, 500);
}

void concurrencpp::tests::test_timer_delay(){
	test_timer_delay_impl().get();
}

void concurrencpp::tests::test_timer_assignment_operator_empty_to_empty(){
	concurrencpp::timer timer1(1000, 1000, [] {}), timer2(1000, 1000, [] {});
	auto ignored1 = std::move(timer1), ignored2 = std::move(timer2);
	ignored1.cancel();
	ignored2.cancel();

	timer1 = std::move(timer2);
	assert_false(timer1.valid());
	assert_false(timer2.valid());
}

void concurrencpp::tests::test_timer_assignment_operator_non_empty_to_non_empty(){
	std::atomic_size_t counter1 = 0, counter2 = 0;
	concurrencpp::timer timer1(500, 500, [&counter1] {++counter1; }),
		timer2(500, 500, [&counter2] { ++counter2; });

	timer1 = std::move(timer2);

	assert_true(timer1.valid());
	assert_false(timer2.valid());

	std::this_thread::sleep_for(std::chrono::seconds(3));

	assert_same(counter1, 0);
	assert_true(counter2 > 0);
}

void concurrencpp::tests::test_timer_assignment_operator_empty_to_non_empty(){
	std::atomic_size_t counter = 0;
	concurrencpp::timer timer1(1000, 1000, [] {}), timer2(500, 500, [&] {++counter; });
	
	auto ignored1 = std::move(timer1);
	ignored1.cancel();

	timer2 = std::move(timer1);

	assert_false(timer1.valid());
	assert_false(timer2.valid());

	std::this_thread::sleep_for(std::chrono::seconds(3));

	assert_same(counter, 0);
}

void concurrencpp::tests::test_timer_assignment_operator_non_empty_to_empty(){
	std::atomic_size_t counter = 0;
	concurrencpp::timer timer1(1000, 1000, [] {}), timer2(500, 500, [&] {++counter; });

	auto ignored1 = std::move(timer1);
	ignored1.cancel();

	timer1 = std::move(timer2);

	assert_true(timer1.valid());
	assert_false(timer2.valid());

	std::this_thread::sleep_for(std::chrono::seconds(3));

	assert_true(counter > 0);
}

void concurrencpp::tests::test_timer_assignment_operator_assign_to_self(){
	std::atomic_size_t counter = 0;
	concurrencpp::timer timer(500, 500, [&] {++counter; });

	timer = std::move(timer);

	assert_true(timer.valid());

	std::this_thread::sleep_for(std::chrono::seconds(3));

	assert_true(counter > 0);
}

void concurrencpp::tests::test_timer_assignment_operator(){
	test_timer_assignment_operator_empty_to_empty();
	test_timer_assignment_operator_non_empty_to_non_empty();
	test_timer_assignment_operator_empty_to_non_empty();
	test_timer_assignment_operator_non_empty_to_empty();
	test_timer_assignment_operator_assign_to_self();
}

void concurrencpp::tests::test_timer(){
	warm_up_environment();

	test_timer_constructor();
	test_many_timers();
	test_timer_destructor();

	test_timer_cancel();
	test_timer_valid();
	test_timer_set_frequency();
	test_timer_once();
	test_timer_delay();
	test_timer_assignment_operator();
}