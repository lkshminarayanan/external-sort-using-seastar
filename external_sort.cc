#include "external_sort.hh"

#include <seastar/core/sharded.hh>

#include "app_config.hh"
#include "first_pass_service.hh"
#include "second_pass_service.hh"
#include "verify_service.hh"

seastar::future<> external_sort(const app_config &config) {
    logger.info("Starting external sort on file : {}", config.input_filename);

    seastar::sharded<first_pass_service> fps;
    seastar::sharded<second_pass_service> sps;
    seastar::sharded<second_pass_service> final_ps;
    seastar::sharded<verify_service> vs;

    seastar::file input_file, output_file;

    try {

        input_file = co_await seastar::open_file_dma(config.input_filename,
                                                     seastar::open_flags::ro);

        // initialize the first pass service across shards
        co_await fps.start(input_file.dup(), config.temp_working_dir);

        logger.info("Running first pass");

        // run the first pass
        co_await fps.invoke_on_all([](first_pass_service &local_service) {
            return local_service.run();
        });

        logger.info("Completed first pass");
        logger.info("Running second pass");

        // initialize the second pass service across shards
        auto get_num_of_files = [](const first_pass_service &fps) {
            return fps.get_total_files();
        };
        co_await sps.start(
            config.temp_working_dir,
            seastar::sharded_parameter(get_num_of_files, std::ref(fps)));

        // run the second pass
        co_await sps.invoke_on_all([](second_pass_service &local_service) {
            return local_service.run();
        });

        logger.info("Completed second pass");
        logger.info("Running a final pass merging all intermediate files into "
                    "a single sorted file");

        // initialize the final pass
        co_await final_ps.start(config.temp_working_dir, seastar::smp::count,
                                config.output_filename);
        // run it either locally or on another shard if available
        co_await final_ps.invoke_on((seastar::smp::count > 1 ? 1 : 0),
                                    [](second_pass_service &local_service) {
                                        return local_service.run();
                                    });

        logger.info("Completed sorting the given file");
        logger.info("Sorted file is stored at : {}", config.output_filename);

        if (config.verify_results) {
            output_file = co_await seastar::open_file_dma(
                config.output_filename, seastar::open_flags::ro);

            // initialize the verify service across shards
            co_await vs.start(output_file.dup());

            logger.info("Verifying the sorted result file");

            if (co_await input_file.size() != co_await output_file.size()) {
                throw verification_exception(
                    "sorted result file has a different size than the input "
                    "file");
            }

            // run the verify service
            co_await vs.invoke_on_all([](verify_service &local_service) {
                return local_service.run();
            });

            // none of the shards threw an exception => verification succeeded;
            logger.info("Result file verification succeeded!");
        }

    } catch (verification_exception ex) {
        logger.error("Result file verification failed : {}", ex.what());

    } catch (...) {
        logger.error("external sort failed with following error : {}",
                     std::current_exception());
    }

    // cleanup
    if (input_file) {
        co_await input_file.close();
    }
    if (output_file) {
        co_await output_file.close();
    }
    co_await fps.stop();
    co_await sps.stop();
    co_await final_ps.stop();
    co_await vs.stop();
}
