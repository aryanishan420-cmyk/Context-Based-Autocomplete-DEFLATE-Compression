// Command line for the DEFLATE implementation.
//
//   deflate compress   input.txt -o out.gz --format gzip --level 9
//   deflate decompress out.gz -o roundtrip.txt
//   deflate inspect    input.txt --level 6
//   deflate bench      input.txt
#include <chrono>
#include <iomanip>
#include <iostream>

#include "common/cli.hpp"
#include "deflate/deflate.hpp"

using namespace deflate;

namespace {

void usage() {
  std::cout << R"(DEFLATE: LZ77 sliding window + Huffman coding (RFC 1951)

usage: deflate <command> [input] [options]

commands:
  compress    Compress a file (or stdin when no input is given)
                --level N        0-9, higher searches harder (default 6)
                --format FMT     raw | zlib | gzip (default gzip)
                -o, --out FILE   output path (default stdout)
                --stats          print block-by-block statistics

  decompress  Decompress a file (or stdin)
                --format FMT     raw | zlib | gzip | auto (default auto)
                -o, --out FILE   output path (default stdout)

  inspect     Compress in memory and report how the stream was built
                --level N        0-9 (default 6)

  bench       Compress at every level and report ratio and speed
                --format FMT     raw | zlib | gzip (default gzip)
)";
}

Container parseFormat(const std::string& name, bool allow_auto = false) {
  if (name == "raw") return Container::Raw;
  if (name == "zlib") return Container::Zlib;
  if (name == "gzip" || name == "gz") return Container::Gzip;
  if (allow_auto && name == "auto") return Container::Raw;  // replaced by detection
  throw std::runtime_error("unknown format: " + name);
}

std::vector<uint8_t> readInput(const cli::Args& args) {
  if (args.positional().empty()) return cli::readStdin();
  return cli::readBinary(args.positional().front());
}

void writeOutput(const cli::Args& args, const std::vector<uint8_t>& data) {
  const std::string out = args.get("out", args.get("o"));
  if (out.empty()) {
    std::cout.write(reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size()));
    return;
  }
  cli::writeBinary(out, data);
}

int cmdCompress(const cli::Args& args) {
  const std::vector<uint8_t> input = readInput(args);
  Options opts;
  opts.level = static_cast<int>(args.num("level", 6));
  opts.container = parseFormat(args.get("format", "gzip"));
  if (!args.positional().empty() && opts.container == Container::Gzip) {
    const std::string& path = args.positional().front();
    opts.filename = path.substr(path.find_last_of('/') + 1);
  }

  Report report;
  const auto start = std::chrono::steady_clock::now();
  const std::vector<uint8_t> out = compress(input, opts, &report);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  writeOutput(args, out);

  if (args.has("stats")) {
    std::cerr << std::fixed << std::setprecision(3)
              << "input        " << report.input_bytes << " bytes\n"
              << "output       " << report.output_bytes << " bytes\n"
              << "ratio        " << report.ratio() << " ("
              << std::setprecision(2) << 8.0 * report.ratio() << " bits/byte)\n"
              << "literals     " << report.lz77.literals << "\n"
              << "matches      " << report.lz77.matches << " covering "
              << report.lz77.matched_bytes << " bytes\n"
              << "chain steps  " << report.lz77.chain_steps << "\n"
              << "throughput   " << std::setprecision(2)
              << (secs > 0 ? report.input_bytes / secs / (1024 * 1024) : 0.0) << " MiB/s\n";
  }
  return 0;
}

int cmdDecompress(const cli::Args& args) {
  const std::vector<uint8_t> input = readInput(args);
  const std::string fmt = args.get("format", "auto");
  const Container container = (fmt == "auto") ? detectContainer(input.data(), input.size())
                                              : parseFormat(fmt);
  writeOutput(args, decompress(input, container));
  return 0;
}

int cmdInspect(const cli::Args& args) {
  const std::vector<uint8_t> input = readInput(args);
  Options opts;
  opts.level = static_cast<int>(args.num("level", 6));
  opts.container = Container::Raw;

  Report report;
  compress(input, opts, &report);

  std::cout << "level " << opts.level << ", " << report.input_bytes << " -> "
            << report.output_bytes << " bytes (ratio " << std::fixed
            << std::setprecision(4) << report.ratio() << ")\n\n"
            << "LZ77\n"
            << "  literals        " << report.lz77.literals << "\n"
            << "  matches         " << report.lz77.matches << "\n"
            << "  bytes matched   " << report.lz77.matched_bytes << "\n"
            << "  avg match len   " << std::setprecision(2)
            << (report.lz77.matches ? static_cast<double>(report.lz77.matched_bytes) /
                                          static_cast<double>(report.lz77.matches)
                                    : 0.0)
            << "\n  chain steps     " << report.lz77.chain_steps << "\n\n"
            << "blocks (" << report.blocks.size() << ")\n"
            << "  " << std::left << std::setw(4) << "#" << std::setw(10) << "type"
            << std::right << std::setw(10) << "tokens" << std::setw(12) << "raw bytes"
            << std::setw(12) << "out bytes" << std::setw(10) << "bits/byte" << "\n";

  size_t i = 0;
  for (const BlockReport& b : report.blocks) {
    std::cout << "  " << std::left << std::setw(4) << i++ << std::setw(10) << b.type
              << std::right << std::setw(10) << b.tokens << std::setw(12) << b.raw_bytes
              << std::setw(12) << (b.bits + 7) / 8 << std::setw(10) << std::setprecision(3)
              << (b.raw_bytes ? static_cast<double>(b.bits) / static_cast<double>(b.raw_bytes) : 0.0)
              << "\n";
    if (i >= 40 && report.blocks.size() > 42) {
      std::cout << "  ... " << (report.blocks.size() - i) << " more blocks\n";
      break;
    }
  }
  return 0;
}

int cmdBench(const cli::Args& args) {
  const std::vector<uint8_t> input = readInput(args);
  const Container container = parseFormat(args.get("format", "gzip"));

  std::cout << input.size() << " bytes\n\n"
            << std::left << std::setw(7) << "level" << std::right << std::setw(12)
            << "output" << std::setw(10) << "ratio" << std::setw(12) << "bits/byte"
            << std::setw(14) << "compress" << std::setw(14) << "decompress"
            << std::setw(10) << "matches" << "\n"
            << std::string(79, '-') << "\n";

  for (int level = 0; level <= 9; ++level) {
    Options opts;
    opts.level = level;
    opts.container = container;

    Report report;
    auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint8_t> out = compress(input, opts, &report);
    auto t1 = std::chrono::steady_clock::now();
    const std::vector<uint8_t> back = decompress(out, container);
    auto t2 = std::chrono::steady_clock::now();
    if (back != input) throw std::runtime_error("round-trip mismatch at level " +
                                                std::to_string(level));

    const double comp_s = std::chrono::duration<double>(t1 - t0).count();
    const double dec_s = std::chrono::duration<double>(t2 - t1).count();
    const double mib = static_cast<double>(input.size()) / (1024 * 1024);
    std::cout << std::left << std::setw(7) << level << std::right << std::setw(12)
              << out.size() << std::fixed << std::setprecision(4) << std::setw(10)
              << report.ratio() << std::setprecision(3) << std::setw(12)
              << 8.0 * report.ratio() << std::setprecision(1) << std::setw(11)
              << (comp_s > 0 ? mib / comp_s : 0.0) << " MiB/s" << std::setw(11)
              << (dec_s > 0 ? mib / dec_s : 0.0) << " MiB/s" << std::setw(10)
              << report.lz77.matches << "\n";
  }
  std::cout << "\nevery level round-tripped through the decoder\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  const std::string command = argv[1];
  const cli::Args args(argc, argv, 2);
  try {
    if (command == "compress" || command == "c") return cmdCompress(args);
    if (command == "decompress" || command == "d") return cmdDecompress(args);
    if (command == "inspect") return cmdInspect(args);
    if (command == "bench") return cmdBench(args);
    if (command == "help" || command == "--help" || command == "-h") {
      usage();
      return 0;
    }
    std::cerr << "unknown command: " << command << "\n\n";
    usage();
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
