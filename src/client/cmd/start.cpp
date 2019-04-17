/*
 * Copyright (C) 2017-2019 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "start.h"
#include "common_cli.h"
#include "exec.h"

#include "animated_spinner.h"

#include <multipass/cli/argparser.h>
#include <multipass/constants.h>

#include <fmt/ostream.h>

#include <cassert>

namespace mp = multipass;
namespace cmd = multipass::cmd;
using RpcMethod = mp::Rpc::Stub;

namespace
{
constexpr auto deleted_error_fmt =
    "Instance '{}' deleted. Use 'recover' to recover it or 'purge' to permanently delete it.\n";
constexpr auto absent_error_fmt = "Instance '{}' does not exist.\n";
constexpr auto unknown_error_fmt = "Error on instance '{}'.\n";
}

mp::ReturnCode cmd::Start::run(mp::ArgParser* parser)
{
    auto ret = parse_args(parser);
    if (ret != ParseCode::Ok)
    {
        return parser->returnCodeFrom(ret);
    }

    AnimatedSpinner spinner{cout};

    auto on_success = [&spinner, this](mp::StartReply& reply) {
        spinner.stop();
        if (term->is_live() && update_available(reply.update_info()))
            cout << update_notice(reply.update_info());
        return ReturnCode::Ok;
    };

    auto on_failure = [this, &spinner, parser](grpc::Status& status) {
        spinner.stop();

        std::string details;
        if (status.error_code() == grpc::StatusCode::ABORTED && !status.error_details().empty())
        {
            mp::StartError start_error;
            start_error.ParseFromString(status.error_details());

            for (const auto& pair : start_error.instance_errors())
            {
                const auto* err_fmt = unknown_error_fmt;
                if (pair.second == mp::StartError::INSTANCE_DELETED)
                    err_fmt = deleted_error_fmt;
                else if (pair.second == mp::StartError::DOES_NOT_EXIST)
                {
                    if (pair.first != petenv_name)
                        err_fmt = absent_error_fmt;
                    else
                        continue;
                }

                fmt::format_to(std::back_inserter(details), err_fmt, pair.first);
            }

            if (details.empty())
            {
                assert(start_error.instance_errors_size() == 1 &&
                       std::cbegin(start_error.instance_errors())->first == petenv_name);
                return run_cmd_and_retry({"multipass", "launch", "--name", petenv_name}, parser, cout, cerr); /*
                                    TODO replace with create, so that all instances are started in a single go */
            }
        }

        return standard_failure_handler_for(name(), cerr, status, details);
    };

    auto streaming_callback = [&spinner](mp::StartReply& reply) {
        spinner.stop();
        spinner.start(reply.reply_message());
    };

    request.set_verbosity_level(parser->verbosityLevel());

    ReturnCode return_code;
    do
    {
        spinner.start(instance_action_message_for(request.instance_names(), "Starting "));
    } while ((return_code = dispatch(&RpcMethod::start, request, on_success, on_failure, streaming_callback)) ==
             ReturnCode::Retry);

    return return_code;
}

std::string cmd::Start::name() const
{
    return "start";
}

QString cmd::Start::short_help() const
{
    return QStringLiteral("Start instances");
}

QString cmd::Start::description() const
{
    return QStringLiteral("Start the named instances. Exits with return code 0\n"
                          "when the instances start, or with an error code if\n"
                          "any fail to start.");
}

mp::ParseCode cmd::Start::parse_args(mp::ArgParser* parser)
{
    parser->addPositionalArgument(
        "name",
        QString{"Names of instances to start. If omitted, and without the --all option, '%1' will be assumed."}.arg(
            petenv_name),
        "[<name> ...]");

    QCommandLineOption all_option(all_option_name, "Start all instances");
    parser->addOption(all_option);

    auto status = parser->commandParse(this);
    if (status != ParseCode::Ok)
        return status;

    auto parse_code = check_for_name_and_all_option_conflict(parser, cerr, /*allow_empty=*/true);
    if (parse_code != ParseCode::Ok)
        return parse_code;

    request.mutable_instance_names()->CopyFrom(add_instance_names(parser, /*default_name=*/petenv_name));

    return status;
}
