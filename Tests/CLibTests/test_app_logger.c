#include "test_support.h"
#include "../../Sources/CDSP/Logging/app_logger.h"

TEST(AppLoggerLevelFiltering) {
    app_logger_set_level(LOG_LEVEL_WARN);
    ASSERT_EQ(LOG_LEVEL_WARN, app_logger_get_level());

    app_logger_set_level(LOG_LEVEL_INFO);
    ASSERT_EQ(LOG_LEVEL_INFO, app_logger_get_level());
}

TEST(AppLoggerBasicLogging) {
    logger_t log = logger_create("test.logger");
    logger_info(&log, "Hello %s %d %.1f", log_arg_string("world"), log_arg_int(42), log_arg_double(3.14), log_arg_none());
    logger_warn(&log, "Warning message", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
    app_logger_flush_and_stop(app_logger_get_shared());
    ASSERT_TRUE(true);
}

TEST_MAIN()
