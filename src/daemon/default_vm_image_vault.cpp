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

#include "default_vm_image_vault.h"
#include "json_writer.h"

#include <multipass/exceptions/create_image_exception.h>
#include <multipass/logging/log.h>
#include <multipass/platform.h>
#include <multipass/query.h>
#include <multipass/rpc/multipass.grpc.pb.h>
#include <multipass/url_downloader.h>
#include <multipass/utils.h>
#include <multipass/vm_image.h>
#include <multipass/xz_image_decoder.h>

#include <fmt/format.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>

#include <exception>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto category = "image vault";
constexpr auto instance_db_name = "multipassd-instance-image-records.json";
constexpr auto image_db_name = "multipassd-image-records.json";

auto filename_for(const QString& path)
{
    QFileInfo file_info(path);
    return file_info.fileName();
}

auto query_to_json(const mp::Query& query)
{
    QJsonObject json;
    json.insert("release", QString::fromStdString(query.release));
    json.insert("persistent", query.persistent);
    json.insert("remote_name", QString::fromStdString(query.remote_name));
    json.insert("query_type", static_cast<int>(query.query_type));
    return json;
}

auto image_to_json(const mp::VMImage& image)
{
    QJsonObject json;
    json.insert("path", image.image_path);
    json.insert("kernel_path", image.kernel_path);
    json.insert("initrd_path", image.initrd_path);
    json.insert("id", QString::fromStdString(image.id));
    json.insert("original_release", QString::fromStdString(image.original_release));
    json.insert("current_release", QString::fromStdString(image.current_release));
    json.insert("release_date", QString::fromStdString(image.release_date));

    QJsonArray aliases;
    for (const auto& alias : image.aliases)
    {
        QJsonObject alias_entry;
        alias_entry.insert("alias", QString::fromStdString(alias));
        aliases.append(alias_entry);
    }
    json.insert("aliases", aliases);

    return json;
}

auto record_to_json(const mp::VaultRecord& record)
{
    QJsonObject json;
    json.insert("image", image_to_json(record.image));
    json.insert("query", query_to_json(record.query));
    json.insert("last_accessed", static_cast<qint64>(record.last_accessed.time_since_epoch().count()));
    return json;
}

std::unordered_map<std::string, mp::VaultRecord> load_db(const QString& db_name)
{
    QFile db_file{db_name};
    auto opened = db_file.open(QIODevice::ReadOnly);
    if (!opened)
        return {};

    QJsonParseError parse_error;
    auto doc = QJsonDocument::fromJson(db_file.readAll(), &parse_error);
    if (doc.isNull())
        return {};

    auto records = doc.object();
    if (records.isEmpty())
        return {};

    std::unordered_map<std::string, mp::VaultRecord> reconstructed_records;
    for (auto it = records.constBegin(); it != records.constEnd(); ++it)
    {
        auto key = it.key().toStdString();
        auto record = it.value().toObject();
        if (record.isEmpty())
            return {};

        auto image = record["image"].toObject();
        if (image.isEmpty())
            return {};

        auto image_path = image["path"].toString();
        if (image_path.isNull())
            return {};

        auto kernel_path = image["kernel_path"].toString();
        auto initrd_path = image["initrd_path"].toString();
        auto image_id = image["id"].toString().toStdString();
        auto original_release = image["original_release"].toString().toStdString();
        auto current_release = image["current_release"].toString().toStdString();
        auto release_date = image["release_date"].toString().toStdString();

        std::vector<std::string> aliases;
        for (const auto& entry : image["aliases"].toArray())
        {
            auto alias = entry.toObject()["alias"].toString().toStdString();
            aliases.push_back(alias);
        }

        auto query = record["query"].toObject();
        if (query.isEmpty())
            return {};

        auto release = query["release"].toString();
        auto persistent = query["persistent"];
        if (!persistent.isBool())
            return {};
        auto remote_name = query["remote_name"].toString();
        auto query_type = static_cast<mp::Query::Type>(query["type"].toInt());

        std::chrono::system_clock::time_point last_accessed;
        auto last_accessed_count = static_cast<qint64>(record["last_accessed"].toDouble());
        if (last_accessed_count == 0)
        {
            last_accessed = std::chrono::system_clock::now();
        }
        else
        {
            auto duration = std::chrono::system_clock::duration(last_accessed_count);
            last_accessed = std::chrono::system_clock::time_point(duration);
        }

        reconstructed_records[key] = {
            {image_path, kernel_path, initrd_path, image_id, original_release, current_release, release_date, aliases},
            {"", release.toStdString(), persistent.toBool(), remote_name.toStdString(), query_type},
            last_accessed};
    }
    return reconstructed_records;
}

QString copy(const QString& file_name, const QDir& output_dir)
{
    if (file_name.isEmpty())
        return {};

    if (!QFileInfo::exists(file_name))
        throw std::runtime_error(fmt::format("{} missing", file_name.toStdString()));

    QFileInfo info{file_name};
    const auto source_name = info.fileName();
    auto new_path = output_dir.filePath(source_name);
    QFile::copy(file_name, new_path);
    return new_path;
}

void delete_file(const QString& path)
{
    QFile file{path};
    file.remove();
}

void remove_source_images(const mp::VMImage& source_image, const mp::VMImage& prepared_image)
{
    // The prepare phase may have been a no-op, check and only remove source images
    if (source_image.image_path != prepared_image.image_path)
        delete_file(source_image.image_path);
    if (source_image.kernel_path != prepared_image.kernel_path)
        delete_file(source_image.kernel_path);
    if (source_image.initrd_path != prepared_image.initrd_path)
        delete_file(source_image.initrd_path);
}

void verify_image_download(const mp::Path& image_path, const std::string& image_hash)
{
    QFile image_file(image_path);
    if (!image_file.open(QFile::ReadOnly))
    {
        throw std::runtime_error("Cannot open image file for computing hash");
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&image_file))
    {
        throw std::runtime_error("Cannot read image file to compute hash");
    }

    if (hash.result().toHex().toStdString() != image_hash)
    {
        throw std::runtime_error("Downloaded image hash does not match");
    }
}

class DeleteOnException
{
public:
    explicit DeleteOnException(const mp::Path& path) : file(path)
    {
    }
    ~DeleteOnException()
    {
        if (std::uncaught_exception())
        {
            file.remove();
        }
    }

private:
    QFile file;
};
} // namespace

mp::DefaultVMImageVault::DefaultVMImageVault(std::vector<VMImageHost*> image_hosts, URLDownloader* downloader,
                                             mp::Path cache_dir_path, mp::Path data_dir_path, mp::days days_to_expire)
    : image_hosts{image_hosts},
      url_downloader{downloader},
      cache_dir{QDir(cache_dir_path).filePath("vault")},
      data_dir{QDir(data_dir_path).filePath("vault")},
      instances_dir(data_dir.filePath("instances")),
      images_dir(cache_dir.filePath("images")),
      days_to_expire{days_to_expire},
      prepared_image_records{load_db(cache_dir.filePath(image_db_name))},
      instance_image_records{load_db(data_dir.filePath(instance_db_name))}
{
    for (const auto& image_host : image_hosts)
    {
        for (const auto& remote : image_host->supported_remotes())
        {
            remote_image_host_map[remote] = image_host;
        }
    }
}

mp::VMImage mp::DefaultVMImageVault::fetch_image(const FetchType& fetch_type, const Query& query,
                                                 const PrepareAction& prepare, const ProgressMonitor& monitor)
{
    auto name_entry = instance_image_records.find(query.name);
    if (name_entry != instance_image_records.end())
    {
        const auto& record = name_entry->second;

        return record.image;
    }

    if (query.query_type != Query::Type::Alias)
    {
        if (!mp::platform::is_image_url_supported())
            throw std::runtime_error(fmt::format("http and file based images are not supported"));

        QUrl image_url(QString::fromStdString(query.release));
        VMImage source_image, vm_image;

        if (image_url.isLocalFile())
        {
            if (!QFile::exists(image_url.path()))
                throw std::runtime_error(
                    fmt::format("Custom image `{}` does not exist.", image_url.path().toStdString()));
            source_image.image_path = image_url.path();

            if (source_image.image_path.endsWith(".xz"))
            {
                source_image = extract_image_from(query.name, source_image, monitor);
            }
            else
            {
                source_image = image_instance_from(query.name, source_image);
            }

            if (fetch_type == FetchType::ImageKernelAndInitrd)
            {
                Query kernel_query{query.name, "default", false, "", Query::Type::Alias};
                auto info = info_for(kernel_query);

                source_image = fetch_kernel_and_initrd(info, source_image,
                                                       QFileInfo(source_image.image_path).absoluteDir(), monitor);
            }

            vm_image = prepare(source_image);
            remove_source_images(source_image, vm_image);
        }
        else
        {
            // Generate a sha256 hash based on the URL and use that for the id
            auto hash =
                QCryptographicHash::hash(query.release.c_str(), QCryptographicHash::Sha256).toHex().toStdString();
            auto last_modified = url_downloader->last_modified(image_url);

            auto entry = prepared_image_records.find(hash);
            if (entry != prepared_image_records.end())
            {
                auto& record = entry->second;

                if (last_modified.isValid() && (last_modified.toString().toStdString() == record.image.release_date))
                {
                    record.last_accessed = std::chrono::system_clock::now();
                    auto vm_image = finalize_image_records(query, record.image);
                    return vm_image;
                }
                else
                {
                    source_image = record.image;
                }
            }
            else
            {
                const auto image_filename = filename_for(image_url.path());
                // Attempt to make a sane directory name based on the filename of the image
                const auto image_dir_name = QString("%1-%2")
                                          .arg(image_filename.section(".", 0, image_filename.endsWith(".xz") ? -3 : -2))
                                          .arg(last_modified.toString("yyyyMMdd"));
                const QDir image_dir{mp::utils::make_dir(images_dir, image_dir_name)};

                source_image.id = hash;
                source_image.image_path = image_dir.filePath(image_filename);
            }

            DeleteOnException image_file{source_image.image_path};
            url_downloader->download_to(image_url, source_image.image_path, 0, LaunchProgress::IMAGE, monitor);

            if (fetch_type == FetchType::ImageKernelAndInitrd)
            {
                Query kernel_query{query.name, "default", false, "", Query::Type::Alias};
                auto info = info_for(kernel_query);

                source_image = fetch_kernel_and_initrd(info, source_image,
                                                       QFileInfo(source_image.image_path).absoluteDir(), monitor);
            }

            if (source_image.image_path.endsWith(".xz"))
            {
                source_image = extract_downloaded_image(source_image, monitor);
            }

            vm_image = prepare(source_image);
            vm_image.release_date = last_modified.toString().toStdString();
            prepared_image_records[hash] = {vm_image, query, std::chrono::system_clock::now()};
            remove_source_images(source_image, vm_image);
            persist_image_records();
            vm_image = image_instance_from(query.name, vm_image);
        }

        instance_image_records[query.name] = {vm_image, query, std::chrono::system_clock::now()};
        persist_instance_records();

        return vm_image;
    }
    else
    {
        auto info = info_for(query);

        if (!mp::platform::is_remote_supported(query.remote_name))
            throw std::runtime_error(fmt::format(
                "{} is not a supported remote. Please use `multipass find` for supported images.", query.remote_name));

        if (!mp::platform::is_alias_supported(query.release, query.remote_name))
            throw std::runtime_error(
                fmt::format("{} is not a supported alias. Please use `multipass find` for supported image aliases.",
                            query.release));

        auto id = info.id.toStdString();

        std::unique_lock<decltype(fetch_mutex)> lock{fetch_mutex};
        auto it = in_progress_image_fetches.find(id);
        if (it != in_progress_image_fetches.end())
        {
            auto future = it->second;
            monitor(LaunchProgress::WAITING, -1);
            lock.unlock();

            auto prepared_image = future.result();

            lock.lock();
            prepared_image_records[id].last_accessed = std::chrono::system_clock::now();
            auto vm_image = finalize_image_records(query, prepared_image);
            lock.unlock();

            return vm_image;
        }

        if (!query.name.empty())
        {
            for (auto& record : prepared_image_records)
            {
                if (record.second.query.remote_name != query.remote_name)
                    continue;

                const auto aliases = record.second.image.aliases;
                if (id == record.first || std::find(aliases.cbegin(), aliases.cend(), query.release) != aliases.cend())
                {
                    const auto prepared_image = record.second.image;
                    try
                    {
                        record.second.last_accessed = std::chrono::system_clock::now();
                        auto vm_image = finalize_image_records(query, prepared_image);
                        lock.unlock();

                        return vm_image;
                    }
                    catch (const std::exception& e)
                    {
                        mpl::log(mpl::Level::warning, category,
                                 fmt::format("Cannot create instance image: {}", e.what()));

                        break;
                    }
                }
            }
        }

        const auto image_dir_name = QString("%1-%2").arg(info.release).arg(info.version);
        const QDir image_dir{mp::utils::make_dir(images_dir, image_dir_name)};

        VMImage source_image;
        source_image.id = id;
        source_image.image_path = image_dir.filePath(filename_for(info.image_location));
        source_image.original_release = info.release_title.toStdString();
        for (const auto& alias : info.aliases)
        {
            source_image.aliases.push_back(alias.toStdString());
        }
        DeleteOnException image_file{source_image.image_path};

        auto future = QtConcurrent::run([&]() -> VMImage {
            try
            {
                url_downloader->download_to(info.image_location, source_image.image_path, info.size,
                                            LaunchProgress::IMAGE, monitor);

                monitor(LaunchProgress::VERIFY, -1);
                verify_image_download(source_image.image_path, id);

                if (fetch_type == FetchType::ImageKernelAndInitrd)
                {
                    source_image = fetch_kernel_and_initrd(info, source_image, image_dir, monitor);
                }

                if (source_image.image_path.endsWith(".xz"))
                {
                    source_image = extract_downloaded_image(source_image, monitor);
                }

                auto prepared_image = prepare(source_image);
                remove_source_images(source_image, prepared_image);

                return prepared_image;
            }
            catch (const std::exception& e)
            {
                throw CreateImageException(e.what());
            }
        });

        in_progress_image_fetches[id] = future;
        lock.unlock();

        auto prepared_image = future.result();

        lock.lock();
        prepared_image_records[id] = {prepared_image, query, std::chrono::system_clock::now()};
        auto vm_image = finalize_image_records(query, prepared_image);

        in_progress_image_fetches.erase(id);
        lock.unlock();

        return vm_image;
    }
}

void mp::DefaultVMImageVault::remove(const std::string& name)
{
    const auto& name_entry = instance_image_records.find(name);
    if (name_entry == instance_image_records.end())
        return;

    QDir instance_dir{instances_dir};
    if (instance_dir.cd(QString::fromStdString(name)))
        instance_dir.removeRecursively();

    instance_image_records.erase(name);
    persist_instance_records();
}

bool mp::DefaultVMImageVault::has_record_for(const std::string& name)
{
    return instance_image_records.find(name) != instance_image_records.end();
}

void mp::DefaultVMImageVault::prune_expired_images()
{
    std::vector<decltype(prepared_image_records)::key_type> expired_keys;
    for (const auto& record : prepared_image_records)
    {
        // Expire source images if they aren't persistent and haven't been accessed in 14 days
        if (record.second.query.query_type == Query::Type::Alias && !record.second.query.persistent &&
            record.second.last_accessed + days_to_expire <= std::chrono::system_clock::now())
        {
            mpl::log(
                mpl::Level::info, category,
                fmt::format("Source image {} is expired. Removing it from the cache.\n", record.second.query.release));
            expired_keys.push_back(record.first);
            QFileInfo image_file{record.second.image.image_path};
            if (image_file.exists())
                image_file.dir().removeRecursively();
        }
    }

    for (const auto& key : expired_keys)
        prepared_image_records.erase(key);

    persist_image_records();
}

void mp::DefaultVMImageVault::update_images(const FetchType& fetch_type, const PrepareAction& prepare,
                                            const ProgressMonitor& monitor)
{
    std::vector<decltype(prepared_image_records)::key_type> keys_to_update;
    for (const auto& record : prepared_image_records)
    {
        if (record.second.query.query_type == Query::Type::Alias &&
            record.first.compare(0, record.second.query.release.length(), record.second.query.release) != 0)
        {
            auto info = info_for(record.second.query);
            if (info.id.toStdString() != record.first)
            {
                keys_to_update.push_back(record.first);
            }
        }
    }

    for (const auto& key : keys_to_update)
    {
        const auto& record = prepared_image_records[key];
        mpl::log(mpl::Level::info, category, fmt::format("Updating {} source image to latest", record.query.release));
        fetch_image(fetch_type, record.query, prepare, monitor);
    }
}

mp::VMImage mp::DefaultVMImageVault::extract_image_from(const std::string& instance_name, const VMImage& source_image,
                                                        const ProgressMonitor& monitor)
{
    const auto name = QString::fromStdString(instance_name);
    const QDir output_dir{mp::utils::make_dir(instances_dir, name)};
    QFileInfo file_info{source_image.image_path};
    const auto image_name = file_info.fileName().remove(".xz");
    const auto image_path = output_dir.filePath(image_name);

    VMImage image{source_image};
    image.image_path = image_path;

    XzImageDecoder xz_decoder(source_image.image_path);
    xz_decoder.decode_to(image_path, monitor);

    return image;
}

mp::VMImage mp::DefaultVMImageVault::extract_downloaded_image(const VMImage& source_image,
                                                              const ProgressMonitor& monitor)
{
    VMImage image{source_image};
    XzImageDecoder xz_decoder(image.image_path);
    auto image_path = image.image_path.remove(".xz");

    xz_decoder.decode_to(image_path, monitor);
    delete_file(source_image.image_path);
    image.image_path = image_path;

    return image;
}

mp::VMImage mp::DefaultVMImageVault::image_instance_from(const std::string& instance_name,
                                                         const VMImage& prepared_image)
{
    auto name = QString::fromStdString(instance_name);
    auto output_dir = mp::utils::make_dir(instances_dir, name);

    return {copy(prepared_image.image_path, output_dir),
            copy(prepared_image.kernel_path, output_dir),
            copy(prepared_image.initrd_path, output_dir),
            prepared_image.id,
            prepared_image.original_release,
            prepared_image.current_release,
            prepared_image.release_date,
            {}};
}

mp::VMImage mp::DefaultVMImageVault::fetch_kernel_and_initrd(const VMImageInfo& info, const VMImage& source_image,
                                                             const QDir& image_dir, const ProgressMonitor& monitor)
{
    auto image{source_image};

    image.kernel_path = image_dir.filePath(filename_for(info.kernel_location));
    image.initrd_path = image_dir.filePath(filename_for(info.initrd_location));
    DeleteOnException kernel_file{image.kernel_path};
    DeleteOnException initrd_file{image.initrd_path};
    url_downloader->download_to(info.kernel_location, image.kernel_path, -1, LaunchProgress::KERNEL, monitor);
    url_downloader->download_to(info.initrd_location, image.initrd_path, -1, LaunchProgress::INITRD, monitor);

    return image;
}

mp::VMImage mp::DefaultVMImageVault::finalize_image_records(const Query& query, const VMImage& prepared_image)
{
    VMImage vm_image;

    if (!query.name.empty())
    {
        vm_image = image_instance_from(query.name, prepared_image);
        instance_image_records[query.name] = {vm_image, query, std::chrono::system_clock::now()};
    }

    persist_instance_records();
    persist_image_records();

    return vm_image;
}

mp::VMImageInfo mp::DefaultVMImageVault::info_for(const mp::Query& query)
{
    if (!query.remote_name.empty())
    {
        auto it = remote_image_host_map.find(query.remote_name);
        if (it == remote_image_host_map.end())
            throw std::runtime_error(fmt::format("Remote \"{}\" is unknown.", query.remote_name));

        auto info = it->second->info_for(query);

        if (info != nullopt)
            return *info;
    }
    else
    {
        for (const auto& image_host : image_hosts)
        {
            auto info = image_host->info_for(query);

            if (info)
            {
                return *info;
            }
        }
    }

    throw std::runtime_error(fmt::format("Unable to find an image matching \"{}\"", query.release));
}

namespace
{
template <typename T>
void persist_records(const T& records, const QString& path)
{
    QJsonObject json_records;
    for (const auto& record : records)
    {
        auto key = QString::fromStdString(record.first);
        json_records.insert(key, record_to_json(record.second));
    }
    mp::write_json(json_records, path);
}
} // namespace

void mp::DefaultVMImageVault::persist_instance_records()
{
    persist_records(instance_image_records, data_dir.filePath(instance_db_name));
}

void mp::DefaultVMImageVault::persist_image_records()
{
    persist_records(prepared_image_records, cache_dir.filePath(image_db_name));
}
