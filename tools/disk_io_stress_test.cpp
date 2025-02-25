/*

Copyright (c) 2020, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/session.hpp" // for default_disk_io_constructor
#include "libtorrent/disk_interface.hpp"
#include "libtorrent/settings_pack.hpp"
#include "libtorrent/file_storage.hpp"
#include "libtorrent/flags.hpp"
#include "libtorrent/performance_counters.hpp"
#include "libtorrent/add_torrent_params.hpp"

// TODO: remove this dependency
#include "libtorrent/aux_/path.hpp"

#include <random>
#include <algorithm>
#include <vector>
#include <iostream>
#include <iomanip>

using disk_test_mode_t = lt::flags::bitfield_flag<std::uint8_t, struct disk_test_mode_tag>;

using lt::operator""_bit;
using lt::operator "" _sv;

namespace test_mode {
constexpr disk_test_mode_t sparse = 0_bit;
constexpr disk_test_mode_t even_file_sizes = 1_bit;
constexpr disk_test_mode_t read_random_order = 2_bit;
constexpr disk_test_mode_t flush_files = 3_bit;
}

std::mt19937 random_engine(std::random_device{}());

// TODO: in C++17, use std::filesystem
void remove_all(std::string path)
{
#ifdef TORRENT_WINDOWS
	WIN32_FIND_DATA data;
	HANDLE list = ::FindFirstFile(path.c_str(), &data);
	if (list == INVALID_HANDLE_VALUE)
	{
		::DeleteFile(path.c_str());
		return;
	}

	do
	{
		if (data.cFileName != "."_sv && data.cFileName != ".."_sv)
		{
			remove_all(path + "\\" + data.cFileName);
		}
	} while(FindNextFile(list, &data));
	FindClose(list);
	RemoveDirectory(path.c_str());
#else
	DIR* handle = ::opendir(path.c_str());
	if (handle == nullptr)
	{
		::remove(path.c_str());
		return;
	}

	dirent* de = ::readdir(handle);
	while (de != nullptr)
	{
		if (de->d_name != "."_sv && de->d_name != ".."_sv)
		{
			remove_all(path + "/" + de->d_name);
		}
		de = ::readdir(handle);
	}
	::closedir(handle);
	::remove(path.c_str());
#endif
}

bool check_block_fill(lt::peer_request const& req, char const* buf, int len)
{
	int const v = (static_cast<int>(req.piece) << 8) | ((req.start / lt::default_block_size) & 0xff);
	for (int i = 0; i < len; i += 4)
		if (std::memcmp(buf + i, reinterpret_cast<char const*>(&v), 4) != 0)
		{
			std::cout << "buffer diverged at byte: " << i << '\n';
			return false;
		}
	return true;
}

void generate_block_fill(lt::peer_request const& req, char* buf, int len)
{
	int const v = (static_cast<int>(req.piece) << 8) | ((req.start / lt::default_block_size) & 0xff);
	for (int i = 0; i < len; i += 4)
		std::memcpy(buf + i, reinterpret_cast<char const*>(&v), 4);
}

struct test_case
{
	int num_files;
	int queue_size;
	int num_threads;
	int read_multiplier;
	int file_pool_size;
	disk_test_mode_t flags;
};

int run_test(test_case const& t) try
{
	lt::file_storage fs;

	std::int64_t file_size = (t.flags & test_mode::even_file_sizes)
		? 0x1000
		: 1337;

	int const piece_size = 0x8000;
	int const blocks_per_piece = std::max(1, piece_size / lt::default_block_size);

	std::int64_t off = 0;
	for (int i = 0; i < t.num_files; ++i)
	{
		fs.add_file("test/" + std::to_string(i), file_size);
		std::cout << " test/" << std::setw(2) << i
			<< " size: " << std::setw(10) << file_size
			<< " first piece: (" << (off / piece_size) << "," << (off % piece_size) << ")"
			<< '\n';
		off += file_size;
		file_size *= 2;
	}
	std::int64_t const total_size = fs.total_size();
	int const num_pieces = static_cast<int>((total_size + piece_size - 1) / piece_size);

	std::cout << "                           last piece: ("
		<< (off / piece_size) << "," << (off % piece_size) << ")\n";
	std::cout << "num pieces: " << num_pieces << '\n';

	fs.set_num_pieces(num_pieces);
	fs.set_piece_length(piece_size);
	lt::io_context ioc;
	lt::counters cnt;
	lt::settings_pack pack;
	pack.set_int(lt::settings_pack::aio_threads, t.num_threads);
	pack.set_int(lt::settings_pack::file_pool_size, t.file_pool_size);

	std::unique_ptr<lt::disk_interface> disk_io
		= lt::default_disk_io_constructor(ioc, pack, cnt);

	std::cerr << "RUNNING: "
		<< ((t.flags & test_mode::sparse) ? "s-" : "f-")
		<< ((t.flags & test_mode::even_file_sizes) ? "e-" : "o-")
		<< ((t.flags & test_mode::read_random_order) ? "rr-" : "or-")
		<< ((t.flags & test_mode::flush_files) ? "f-" : "a-")
		<< num_pieces << '-'
		<< t.file_pool_size << '-'
		<< t.queue_size << '-'
		<< t.read_multiplier
		<< ": ";

	// TODO: in C++17, use std::filesystem
	remove_all("scratch-area");

	// TODO: add test mode where some file priorities are 0

	lt::aux::vector<lt::download_priority_t, lt::file_index_t> prios;
	std::string save_path = "./scratch-area";
	lt::storage_params params(fs, nullptr
		, save_path
		, (t.flags & test_mode::sparse) ? lt::storage_mode_sparse : lt::storage_mode_allocate
		, prios
		, lt::sha1_hash("01234567890123456789"));

	lt::storage_holder tor = disk_io->new_torrent(params, {});

	std::vector<lt::peer_request> blocks_to_write;
	for (int p = 0; p < num_pieces; ++p)
	{
		for (int b = 0; b < blocks_per_piece; ++b)
		{
			blocks_to_write.push_back(
				{lt::piece_index_t{p}, b * lt::default_block_size, lt::default_block_size});
		}
	}
	std::shuffle(blocks_to_write.begin(), blocks_to_write.end(), random_engine);

	std::vector<lt::peer_request> blocks_to_read;
	blocks_to_read.reserve(blocks_to_write.size());

	std::vector<char> write_buffer(lt::default_block_size);

	int outstanding = 0;

	lt::add_torrent_params atp;

	disk_io->async_check_files(tor, &atp, lt::aux::vector<std::string, lt::file_index_t>{}
		, [&](lt::status_t, lt::storage_error const&) { --outstanding; });
	++outstanding;
	disk_io->submit_jobs();

	while (outstanding > 0)
	{
		ioc.run_one();
		ioc.restart();
	}

	int job_counter = 0;

	while (!blocks_to_write.empty()
		|| !blocks_to_read.empty()
		|| outstanding > 0)
	{
		for (int i = 0; i < t.read_multiplier; ++i)
		{
			if (!blocks_to_read.empty() && outstanding < t.queue_size)
			{
				auto const req = blocks_to_read.back();
				blocks_to_read.erase(blocks_to_read.end() - 1);

				disk_io->async_read(tor, req
					, [&, req](lt::disk_buffer_holder h, lt::storage_error const& ec)
					{
						--outstanding;
						++job_counter;
						if (ec) throw std::runtime_error("async_read failed " + ec.ec.message());

						int const block_size = std::min((fs.piece_size(req.piece) - req.start), int(h.size()));
						if (!check_block_fill(req, h.data(), block_size))
						{
							std::cerr << "read buffer mismatch: (" << req.piece << ", " << req.start << ")\n";
							throw std::runtime_error("read buffer mismatch!");
						}
					});

				++outstanding;
			}
		}

		if (!blocks_to_write.empty() && outstanding < t.queue_size)
		{
			auto const req = blocks_to_write.back();
			blocks_to_write.erase(blocks_to_write.end() - 1);

			generate_block_fill(req, write_buffer.data(), lt::default_block_size);

			disk_io->async_write(tor, req, write_buffer.data()
				, {}, [&](lt::storage_error const& ec)
				{
					--outstanding;
					++job_counter;
					if (ec) throw std::runtime_error("async_write failed " + ec.ec.message());
				});
			if (t.flags & test_mode::read_random_order)
			{
				std::uniform_int_distribution<> d(0, int(blocks_to_read.size()));
				blocks_to_read.insert(blocks_to_read.begin() + d(random_engine), req);
			}
			else
			{
				blocks_to_read.push_back(req);
			}
			// if read_multiplier > 1, put this block more times in the
			// read queue
			for (int i = 1; i < t.read_multiplier; ++i)
			{
				std::uniform_int_distribution<> d(0, int(blocks_to_read.size()));
				blocks_to_read.insert(blocks_to_read.begin() + d(random_engine), req);
			}

			++outstanding;
		}

		if ((t.flags & test_mode::flush_files) && (job_counter % 500) == 499)
		{
			disk_io->async_release_files(tor, [&]()
				{
					--outstanding;
					++job_counter;
				});
			++outstanding;
		}

		// TODO: add test_mode for async_move_storage
		// TODO: add test_mode for async_hash and async_hash2
		// TODO: add test_mode for abort_hash_jobs
		// TODO: add test_mode for async_delete_files
		// TODO: add test_mode for async_rename_file
		// TODO: add test_mode for async_set_file_priority

		disk_io->submit_jobs();
		if (outstanding >= t.queue_size)
			ioc.run_one();
		else
			ioc.poll();
		ioc.restart();
	}

	disk_io->remove_torrent(tor);

	disk_io->abort(true);

	std::cerr << "OK (" << job_counter << " jobs)\n";
	return 0;
}
catch (std::exception const& e)
{
	std::cerr << "FAILED WITH EXCEPTION: " << e.what() << '\n';
	return 1;
}

void print_usage()
{
	std::cerr << "USAGE: disk_io_stress_test <options>\n"
		"If no options are specified, the default suite of tests are run\n\n"
		"OPTIONS:\n"
		"   alloc\n"
		"      open files in pre-allocate mode\n"
		"   even-size\n"
		"      make test files even multiples of 1 kB\n"
		"   random-read\n"
		"      instead of reading blocks back in the same order they were written,\n"
		"      read them back in random order\n"
		"   flush\n"
		"      issue a 'release-files' disk job every 500 jobs\n"
		"   -f <val>\n"
		"      specifies the number of files to use in the test torrent\n"
		"   -q <val>\n"
		"      specifies the job queue size. i.e. the max number of outstanding\n"
		"      jobs to post to the disk I/O subsystem\n"
		"   -t <val>\n"
		"      specifies the number of disk I/O threads to use\n"
		"   -r <val>\n"
		"      specifies the read multiplier. Each block that's written, is read this many times\n"
		"   -p <val>\n"
		"      specifies the file pool size. This is the number of files to keep open\n"
		;

}

int main(int argc, char const* argv[])
{
	if (argc == 1)
	{
		// the default test suite
		namespace tm = test_mode;

		test_case tests[] = {
			// files, queue, threads, read-mult, pool, flags
			{20, 32, 16, 3, 10, tm::sparse},
			{20, 32, 16, 3, 10, tm::sparse | tm::even_file_sizes},
			{20, 32, 16, 3, 10, tm::sparse | tm::read_random_order},
			{20, 32, 16, 3, 10, tm::sparse | tm::read_random_order | tm::even_file_sizes},
			{20, 32, 16, 3, 10, tm::flush_files | tm::sparse | tm::read_random_order | tm::even_file_sizes},

			// test with small pool size
			{10, 32, 16, 3, 1, tm::sparse | tm::read_random_order},

			// test with many threads pool size
			{10, 32, 64, 3, 9, tm::sparse | tm::read_random_order},
		};

		int ret = 0;
		for (auto const& t : tests)
			ret |= run_test(t);

		return ret;
	}

	// strip program name
	argc -= 1;
	argv += 1;
	test_case tc{20, 32, 16, 3, 10, test_mode::sparse};
	while (argc > 0)
	{
		lt::string_view opt(argv[0]);

		if (opt == "-h" || opt == "--help")
		{
			print_usage();
			return 0;
		}

		if (opt.substr(0, 1) == "-")
		{
			if (argc < 1)
			{
				std::cerr << "missing value associated with \"" << opt << "\"\n";
				print_usage();
				return 1;
			}
			if (opt == "-f")
				tc.num_files = std::atoi(argv[1]);
			else if (opt == "-q")
				tc.queue_size = std::atoi(argv[1]);
			else if (opt == "-t")
				tc.num_threads = std::atoi(argv[1]);
			else if (opt == "-r")
				tc.read_multiplier = std::atoi(argv[1]);
			else if (opt == "-p")
				tc.file_pool_size = std::atoi(argv[1]);
			else
			{
				std::cerr << "unknown option \"" << opt << "\"\n";
				print_usage();
				return 1;
			}

			argc -= 1;
			argv += 1;
		}
		else if (opt == "alloc")
			tc.flags &= ~test_mode::sparse;
		else if (opt == "even-size")
			tc.flags |= test_mode::even_file_sizes;
		else if (opt == "random-read")
			tc.flags |= test_mode::read_random_order;
		else if (opt == "flush")
			tc.flags |= test_mode::flush_files;
		else
		{
			std::cerr << "unknown option \"" << opt << "\"\n";
			print_usage();
			return 1;
		}

		argc -= 1;
		argv += 1;
	}

	return run_test(tc);
}
