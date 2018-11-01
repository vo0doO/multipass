/*
 * Copyright (C) 2018 Canonical, Ltd.
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

#include "dnsmasq_server.h"

#include <multipass/logging/log.h>
#include <multipass/utils.h>

#include <fmt/format.h>

#include <fstream>

#include "dhcp_release_process.h"

namespace mp = multipass;
namespace mpl = multipass::logging;

mp::DNSMasqServer::DNSMasqServer(const Path& path, const QString& bridge_name, const IPAddress& bridge_addr,
                                 const IPAddress& start, const IPAddress& end)
    : data_dir{QDir(path)},
      dnsmasq_cmd{std::make_unique<DNSMasqProcess>(data_dir, bridge_name, bridge_addr, start, end)},
      bridge_name{bridge_name}
{
    QObject::connect(dnsmasq_cmd.get(), &DNSMasqProcess::readyReadStandardError,
                     [this]() { mpl::log(mpl::Level::error, "dnsmasq", dnsmasq_cmd->readAllStandardError().data()); });

    dnsmasq_cmd->start();
    dnsmasq_cmd->waitForStarted();
}

mp::DNSMasqServer::~DNSMasqServer()
{
    dnsmasq_cmd->kill();
    dnsmasq_cmd->waitForFinished();
}

mp::optional<mp::IPAddress> mp::DNSMasqServer::get_ip_for(const std::string& hw_addr)
{
    // DNSMasq leases entries consist of:
    // <lease expiration> <mac addr> <ipv4> <name> * * *
    const auto path = data_dir.filePath("dnsmasq.leases").toStdString();
    const std::string delimiter{" "};
    const int hw_addr_idx{1};
    const int ipv4_idx{2};
    std::ifstream leases_file{path};
    std::string line;
    while (getline(leases_file, line))
    {
        const auto fields = mp::utils::split(line, delimiter);
        if (fields.size() > 2 && fields[hw_addr_idx] == hw_addr)
            return mp::optional<mp::IPAddress>{fields[ipv4_idx]};
    }
    return mp::nullopt;
}

void mp::DNSMasqServer::release_mac(const std::string& hw_addr)
{
    auto ip = get_ip_for(hw_addr);
    if (!ip)
    {
        mpl::log(mpl::Level::warning, "dnsmasq", fmt::format("attempting to release non-existant addr: {}", hw_addr));
        return;
    }

    DHCPReleaseProcess dhcp_release(bridge_name, ip.value(), QString::fromStdString(hw_addr));
    QObject::connect(&dhcp_release, &DHCPReleaseProcess::errorOccurred, [&ip, &hw_addr](QProcess::ProcessError error) {
        mpl::log(mpl::Level::warning, "dnsmasq",
                 fmt::format("failed to release ip addr {} with mac {}", ip.value().as_string(), hw_addr));
    });

    auto log_exit_status = [&ip, &hw_addr](int exit_code, QProcess::ExitStatus exit_status) {
        if (exit_code == 0 && exit_status == QProcess::NormalExit)
            return;

        auto msg = fmt::format("failed to release ip addr {} with mac {}, exit_code: {}", ip.value().as_string(),
                               hw_addr, exit_code);
        mpl::log(mpl::Level::warning, "dnsmasq", msg);
    };
    QObject::connect(&dhcp_release, &DHCPReleaseProcess::finished, log_exit_status);

    dhcp_release.start();
    dhcp_release.waitForFinished();
}
