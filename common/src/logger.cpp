#include <ag_logger.h>
#include <spdlog/sinks/stdout_sinks.h>

static ag::log_level default_log_level = ag::INFO;
static ag::create_logger_cb create_logger_callback =
    [] (const std::string &name) { return spdlog::stdout_logger_mt(name); };

ag::logger ag::create_logger(const std::string &name) {
    ag::logger logger = spdlog::get(name);
    if (logger == nullptr) {
        logger = create_logger_callback(name);
    }
    logger->set_level((spdlog::level::level_enum)default_log_level);
    return logger;
}

void ag::set_default_log_level(ag::log_level lvl) {
    default_log_level = lvl;
}

void ag::set_logger_factory_callback(create_logger_cb cb) {
    create_logger_callback = cb;
}
