#include "Processor.h"
#include "Config.h"
#include "Controller.h"
#include "SpeedyController.h"
#include "Memory.h"
#include "DRAM.h"
#include "Statistics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdlib.h>
#include <functional>
#include <map>

/* Standards */
#include "Gem5Wrapper.h"
#include "DDR3.h"
#include "DDR4.h"
#include "DSARP.h"
#include "GDDR5.h"
#include "LPDDR3.h"
#include "LPDDR4.h"
#include "WideIO.h"
#include "WideIO2.h"
#include "HBM.h"
#include "SALP.h"
#include "ALDRAM.h"
#include "TLDRAM.h"
#include "STTMRAM.h"
#include "PCM.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

using namespace std;
using namespace ramulator;

bool ramulator::warmup_complete = false;

template <typename T>
void run_dramtrace(const Config &configs, Memory<T, Controller> &memory, const char *tracename)
{

  /* initialize DRAM trace */
  Trace trace(tracename);

  /* run simulation */
  bool stall = false, end = false;
  int reads = 0, writes = 0, clks = 0;
  long addr = 0;
  Request::Type type = Request::Type::READ;
  map<int, int> latencies;
  auto read_complete = [&latencies](Request &r)
  { latencies[r.depart - r.arrive]++; };

  Request req(addr, type, read_complete);

  while (!end || memory.pending_requests())
  {
    if (!end && !stall)
    {
      end = !trace.get_dramtrace_request(addr, type);
    }

    if (!end)
    {
      req.addr = addr;
      req.type = type;
      stall = !memory.send(req);
      if (!stall)
      {
        if (type == Request::Type::READ)
          reads++;
        else if (type == Request::Type::WRITE)
          writes++;
      }
    }
    else
    {
      memory.set_high_writeq_watermark(0.0f); // make sure that all write requests in the
                                              // write queue are drained
    }

    memory.tick();
    clks++;
    Stats::curTick++; // memory clock, global, for Statistics
  }
  // This a workaround for statistics set only initially lost in the end
  memory.finish();
  Stats::statlist.printall();
}

const char *file1;
const char *file2;
int fd_w_;

const int MAX_LEN = 1024;
// const RESPONSE_INDEX=17;
char buf[MAX_LEN];

template <typename T>
void run_interactive(const Config &configs, Memory<T, Controller> &memory)
{

  /* run simulation */
  bool stall = false;
  int reads = 0, writes = 0, clks = 0;
  long addr = 0, request_id = 0;
  string op;
  Request::Type type = Request::Type::READ;
  map<int, int> latencies;
  auto read_complete = [&latencies](Request &r)
  {
    latencies[r.depart - r.arrive]++;
    // char buf[1024];
    buf[100] = 'R';
    memcpy(buf + 1, &r.request_id, sizeof(long));
    // write(fd_w_, buf, sizeof(buf));
    //  std::cout << "READ_RESPONSE " << r.request_id << std::endl;
  };

  Request req(addr, type, read_complete);

  int fd_r = open(file1, O_RDONLY);
  if (fd_r == -1)
  {
    perror("open");
    exit(0);
  }

  int fd_w = open(file2, O_WRONLY);
  fd_w_ = fd_w;
  if (fd_w == -1)
  {
    perror("open");
    exit(0);
  }

  memset(buf, '0', sizeof(buf));
  char op_ = '0';
  while (true)
  {
    // OP: END,TICK,READ,WRITE
    // E T R W
    // RESPONE: DONE,STALL,ACCEPT
    // D S A
    read(fd_r, buf, sizeof(buf));
    op_ = buf[0];
    // std::cin >> op;

    // if(op == "END"){//1 END 2 TICK 3 READ 4 WRITE
    if (op_ == 'E')
    {
      break; // 5 DONE 6
    }
    else if (op_ == 'T')
    {
      memory.tick();
      clks++;
      // std::cout << "DONE " << clks << std::endl;
      buf[0] = 'D';
      write(fd_w, buf, sizeof(buf));
      Stats::curTick++; // memory clock, global, for Statistics
    }
    else if (op_ == 'R')
    {
      // std::cin >> request_id >> addr;

      memcpy(&request_id, buf + sizeof(char), sizeof(long));
      memcpy(&addr, buf + sizeof(long) + sizeof(char), sizeof(long));
      req.type = Request::Type::READ;
      req.addr = addr;
      req.request_id = request_id;
      stall = !memory.send(req);
      if (stall)
      {
        // std::cout << "STALL" << std::endl;
        buf[0] = 'S';
        write(fd_w, buf, sizeof(buf));
      }
      else
      {
        // std::cout << "ACCEPT" << std::endl;
        buf[0] = 'A';
        write(fd_w, buf, sizeof(buf));
        reads++;
      }
    }
    else if (op_ == 'W')
    {
      // std::cin >> request_id >> addr;
      memcpy(&request_id, buf + 1, sizeof(long));
      memcpy(&addr, buf + sizeof(long) + sizeof(char), sizeof(long));
      req.type = Request::Type::WRITE;
      req.addr = addr;
      req.request_id = request_id;
      stall = !memory.send(req);
      if (stall)
      {
        // std::cout << "STALL" << std::endl;
        buf[0] = 'S';
        write(fd_w, buf, sizeof(buf));
      }
      else
      {
        // std::cout << "ACCEPT" << std::endl;
        buf[0] = 'A';
        write(fd_w, buf, sizeof(buf));
        writes++;
      }
    }
  }
  memory.set_high_writeq_watermark(0.0f);
  while (memory.pending_requests())
  {
    memory.tick();
    clks++;
    Stats::curTick++; // memory clock, global, for Statistics
  }
  // This a workaround for statistics set only initially lost in the end
  memory.finish();
  Stats::statlist.printall();
}

template <typename T>
void run_cputrace(const Config &configs, Memory<T, Controller> &memory, const std::vector<const char *> &files)
{
  int cpu_tick = configs.get_cpu_tick();
  int mem_tick = configs.get_mem_tick();
  auto send = bind(&Memory<T, Controller>::send, &memory, placeholders::_1);
  Processor proc(configs, files, send, memory);

  long warmup_insts = configs.get_warmup_insts();
  bool is_warming_up = (warmup_insts != 0);

  for (long i = 0; is_warming_up; i++)
  {
    proc.tick();
    Stats::curTick++;
    if (i % cpu_tick == (cpu_tick - 1))
      for (int j = 0; j < mem_tick; j++)
        memory.tick();

    is_warming_up = false;
    for (int c = 0; c < proc.cores.size(); c++)
    {
      if (proc.cores[c]->get_insts() < warmup_insts)
        is_warming_up = true;
    }

    if (is_warming_up && proc.has_reached_limit())
    {
      printf("WARNING: The end of the input trace file was reached during warmup. "
             "Consider changing warmup_insts in the config file. \n");
      break;
    }
  }

  warmup_complete = true;
  printf("Warmup complete! Resetting stats...\n");
  Stats::reset_stats();
  proc.reset_stats();
  assert(proc.get_insts() == 0);

  printf("Starting the simulation...\n");

  int tick_mult = cpu_tick * mem_tick;
  for (long i = 0;; i++)
  {
    if (((i % tick_mult) % mem_tick) == 0)
    { // When the CPU is ticked cpu_tick times,
      // the memory controller should be ticked mem_tick times
      proc.tick();
      Stats::curTick++; // processor clock, global, for Statistics

      if (configs.calc_weighted_speedup())
      {
        if (proc.has_reached_limit())
        {
          break;
        }
      }
      else
      {
        if (configs.is_early_exit())
        {
          if (proc.finished())
            break;
        }
        else
        {
          if (proc.finished() && (memory.pending_requests() == 0))
            break;
        }
      }
    }

    if (((i % tick_mult) % cpu_tick) == 0) // TODO_hasan: Better if the processor ticks the memory controller
      memory.tick();
  }
  // This a workaround for statistics set only initially lost in the end
  memory.finish();
  Stats::statlist.printall();
}

template <typename T>
void start_run(const Config &configs, T *spec, const vector<const char *> &files)
{
  // initiate controller and memory
  int C = configs.get_channels(), R = configs.get_ranks();
  // Check and Set channel, rank number
  spec->set_channel_number(C);
  spec->set_rank_number(R);
  std::vector<Controller<T> *> ctrls;
  for (int c = 0; c < C; c++)
  {
    DRAM<T> *channel = new DRAM<T>(spec, T::Level::Channel);
    channel->id = c;
    channel->regStats("");
    Controller<T> *ctrl = new Controller<T>(configs, channel);
    ctrls.push_back(ctrl);
  }
  Memory<T, Controller> memory(configs, ctrls);

  assert(files.size() != 0);
  if (configs["trace_type"] == "CPU")
  {
    run_cputrace(configs, memory, files);
  }
  else if (configs["trace_type"] == "DRAM")
  {
    run_dramtrace(configs, memory, files[0]);
  }
  else if (configs["trace_type"] == "INTERACTIVE")
  {
    run_interactive(configs, memory);
  }
}

int main(int argc, const char *argv[])
{
  if (argc < 2)
  {
    printf("Usage: %s <configs-file> --mode=cpu,dram [--stats <filename>] <trace-filename1> <trace-filename2>\n"
           "Example: %s ramulator-configs.cfg --mode=cpu cpu.trace cpu.trace\n",
           argv[0], argv[0]);
    return 0;
  }

  argc-=2;

  file1=argv[3];
  file2=argv[4];

  // printf("%s \n",file1);
  // getchar();

  Config configs(argv[1]);

  const std::string &standard = configs["standard"];
  assert(standard != "" || "DRAM standard should be specified.");

  const char *trace_type = strstr(argv[2], "=");
  trace_type++;
  int trace_start = 3;
  if (strcmp(trace_type, "cpu") == 0)
  {
    configs.add("trace_type", "CPU");
  }
  else if (strcmp(trace_type, "dram") == 0)
  {
    configs.add("trace_type", "DRAM");
  }
  else if (strcmp(trace_type, "interactive") == 0)
  {
    configs.add("trace_type", "INTERACTIVE");
    trace_start = 2;
  }
  else
  {
    printf("invalid trace type: %s\n", trace_type);
    assert(false);
  }

  string stats_out;
  if (strcmp(argv[trace_start], "--stats") == 0)
  {
    Stats::statlist.output(argv[trace_start + 1]);
    stats_out = argv[trace_start + 1];
    trace_start += 2;
  }
  else
  {
    Stats::statlist.output(standard + ".stats");
    stats_out = standard + string(".stats");
  }

  // A separate file defines mapping for easy config.
  if (strcmp(argv[trace_start], "--mapping") == 0)
  {
    configs.add("mapping", argv[trace_start + 1]);
    trace_start += 2;
  }
  else
  {
    configs.add("mapping", "defaultmapping");
  }

  std::vector<const char *> files(&argv[trace_start], &argv[argc]);
  configs.set_core_num(argc - trace_start);

  if (standard == "DDR3")
  {
    DDR3 *ddr3 = new DDR3(configs["org"], configs["speed"]);
    start_run(configs, ddr3, files);
  }
  else if (standard == "DDR4")
  {
    DDR4 *ddr4 = new DDR4(configs["org"], configs["speed"]);
    start_run(configs, ddr4, files);
  }
  else if (standard == "SALP-MASA")
  {
    SALP *salp8 = new SALP(configs["org"], configs["speed"], "SALP-MASA", configs.get_subarrays());
    start_run(configs, salp8, files);
  }
  else if (standard == "LPDDR3")
  {
    LPDDR3 *lpddr3 = new LPDDR3(configs["org"], configs["speed"]);
    start_run(configs, lpddr3, files);
  }
  else if (standard == "LPDDR4")
  {
    // total cap: 2GB, 1/2 of others
    LPDDR4 *lpddr4 = new LPDDR4(configs["org"], configs["speed"]);
    start_run(configs, lpddr4, files);
  }
  else if (standard == "GDDR5")
  {
    GDDR5 *gddr5 = new GDDR5(configs["org"], configs["speed"]);
    start_run(configs, gddr5, files);
  }
  else if (standard == "HBM")
  {
    HBM *hbm = new HBM(configs["org"], configs["speed"]);
    start_run(configs, hbm, files);
  }
  else if (standard == "WideIO")
  {
    // total cap: 1GB, 1/4 of others
    WideIO *wio = new WideIO(configs["org"], configs["speed"]);
    start_run(configs, wio, files);
  }
  else if (standard == "WideIO2")
  {
    // total cap: 2GB, 1/2 of others
    WideIO2 *wio2 = new WideIO2(configs["org"], configs["speed"], configs.get_channels());
    wio2->channel_width *= 2;
    start_run(configs, wio2, files);
  }
  else if (standard == "STTMRAM")
  {
    STTMRAM *sttmram = new STTMRAM(configs["org"], configs["speed"]);
    start_run(configs, sttmram, files);
  }
  else if (standard == "PCM")
  {
    PCM *pcm = new PCM(configs["org"], configs["speed"]);
    start_run(configs, pcm, files);
  }
  // Various refresh mechanisms
  else if (standard == "DSARP")
  {
    DSARP *dsddr3_dsarp = new DSARP(configs["org"], configs["speed"], DSARP::Type::DSARP, configs.get_subarrays());
    start_run(configs, dsddr3_dsarp, files);
  }
  else if (standard == "ALDRAM")
  {
    ALDRAM *aldram = new ALDRAM(configs["org"], configs["speed"]);
    start_run(configs, aldram, files);
  }
  else if (standard == "TLDRAM")
  {
    TLDRAM *tldram = new TLDRAM(configs["org"], configs["speed"], configs.get_subarrays());
    start_run(configs, tldram, files);
  }

  printf("Simulation done. Statistics written to %s\n", stats_out.c_str());

  return 0;
}
