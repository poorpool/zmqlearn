#include "3rdparty/zmq.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <mpi.h>
#include <string>
#include <sys/time.h>
#include <thread>
#include <vector>

using std::string;
using std::vector;

constexpr int64_t kServerRuntimeS = 5; // 运行时间（秒）
constexpr int kArraySize = 1024;       // 资源数组大小
const string kIPCEndpointPrefix = "ipc:///tmp/zmq_ipc";

int mpi_rank;
int mpi_size;

int64_t GetUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (tv.tv_sec * 1000000) + tv.tv_usec;
}

struct Request {
  int req_id_;    // 递增请求号
  int which_;     // 资源数组中的索引
  int user_data_; // 随机生成数
};

void server() {
  // 1. 创建上下文（析构时会自动关闭）
  zmq::context_t context(mpi_size -
                         1); // 也许每个对等端都来一个 I/O 线程会比较好？

  // 2. 创建 socket 并绑定
  // REQ 发给 client；PULL 接收 client 的 PUSH；PUB 发给 client 终止信息
  vector<zmq::socket_t> req_sockets;
  for (int i = 1; i < mpi_size; i++) {
    req_sockets.emplace_back(
        context,
        ZMQ_REQ); // Sockets in cppzmq can not be copied (they can be moved)
    req_sockets.back().bind(kIPCEndpointPrefix + "_req_" + std::to_string(i));
  }
  zmq::socket_t pub_socket(context, ZMQ_PUB);
  pub_socket.bind(kIPCEndpointPrefix + "_pub");
  zmq::socket_t pull_socket(context, ZMQ_PULL);
  pull_socket.set(zmq::sockopt::rcvhwm, 1000000); // 感觉设大点挺好的，防止阻塞
  pull_socket.bind(kIPCEndpointPrefix + "_pull");
  FILE *fp = fopen("/tmp/cyxtestserver", "w");

  // 3. 发起 workload
  vector<int> resource(kArraySize, 0);
  int req_id = 0;
  int req_sent = 0;
  int pull_received = 0;
  int rep_received = 0;

  int64_t start_time = GetUs();
  while (true) {
    if (GetUs() - start_time >= kServerRuntimeS * 1000000) {
      std::string terminate_msg = "TERMINATE";
      pub_socket.send(zmq::buffer(terminate_msg), zmq::send_flags::none);
      break;
    }

    // zmq 的 send 是“异步”的。为了保障数据始终有效，最好等到
    Request request;
    request.req_id_ = ++req_id;
    request.which_ = rand() % kArraySize;
    request.user_data_ = rand();
    fprintf(fp, "req_id = %d, which = %d, user_data = %d\n", request.req_id_,
            request.which_, request.user_data_);
    resource[request.which_] += mpi_size - 1; // 模拟负载的引用计数

    for (auto &req_socket : req_sockets) {
      // 也许我们压根就不需要 zmq::message_t，直接用 buffer 就行
      auto send_res = req_socket.send(
          zmq::const_buffer(&request, sizeof(Request)), zmq::send_flags::none);
      if (send_res != sizeof(Request)) {
        printf("send failed\n");
        exit(-1);
      }
      req_sent++;
    }
    for (auto &req_socket : req_sockets) {
      zmq::message_t response_msg;
      // 收点零长度 rep，确保对面收到了请求
      auto recv_res = req_socket.recv(response_msg, zmq::recv_flags::none);
      if (!recv_res.has_value()) {
        printf("recv failed\n");
        exit(-1);
      }
      rep_received++;
      // 其实没有太大必要用 poller。因为用了以后还得检查每个 event
      // 有无变状态，那还不如直接轮询呢
    }

    // std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int which;
    do {
      // 也许我们压根就不需要 zmq::message_t，直接用 buffer 就行
      auto pull_res = pull_socket.recv(zmq::mutable_buffer(&which, sizeof(int)),
                                       zmq::recv_flags::dontwait);
      if (pull_res.has_value()) {
        if (pull_res->size != sizeof(int)) {
          printf("pull size error\n");
          exit(-1);
        }
        resource[which]--;
        fprintf(fp, "pull which = %d\n", which);
        pull_received++;
      } else {
        break;
      }
    } while (true);
  }

  int which;
  while (pull_received < req_sent) {
    // 也许我们压根就不需要 zmq::message_t，直接用 buffer 就行
    auto pull_res = pull_socket.recv(zmq::mutable_buffer(&which, sizeof(int)),
                                     zmq::recv_flags::none);
    if (!pull_res.has_value()) {
      printf("pull failed\n");
      exit(-1);
    }
    if (pull_res->size != sizeof(int)) {
      printf("pull size error\n");
      exit(-1);
    }
    resource[which]--;
    fprintf(fp, "pull which = %d\n", which);
    pull_received++;
  }

  fclose(fp);
  for (auto &req_socket : req_sockets) {
    req_socket.close();
  }
  pull_socket.close();
  pub_socket.close();
  context.close();

  for (const auto &x : resource) {
    if (x != 0) {
      printf("resource leak\n");
      exit(-1);
    }
  }

  std::cout << "Server Stats: REQ Sent = " << req_sent
            << ", REP Received = " << rep_received
            << ", PULL Received = " << pull_received << std::endl;
}

void client() {
  zmq::context_t context(1);
  zmq::socket_t rep_socket(context, ZMQ_REP);
  zmq::socket_t push_socket(context, ZMQ_PUSH);
  push_socket.set(zmq::sockopt::sndhwm, 1000000); // 感觉设大点挺好的，防止阻塞
  zmq::socket_t sub_socket(context, ZMQ_SUB);

  rep_socket.connect(kIPCEndpointPrefix + "_req_" + std::to_string(mpi_rank));
  push_socket.connect(kIPCEndpointPrefix + "_pull");
  sub_socket.connect(kIPCEndpointPrefix + "_pub");
  sub_socket.set(zmq::sockopt::subscribe, "");

  FILE *fp = fopen(
      (string("/tmp/cyxtestclient") + std::to_string(mpi_rank)).c_str(), "w");

  int rep_sent = 0;
  int push_sent = 0;

  Request request;
  while (true) {
    // 在学习代码中，这么写可以，反正 client 没有其他事情干
    // 在 OnionCache 中，这么写感觉没有必要，反正每个都要检查，还不如直接
    // dontwait 查看
    zmq::pollitem_t items[] = {
        {static_cast<void *>(rep_socket), 0, ZMQ_POLLIN, 0},
        {static_cast<void *>(sub_socket), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, 2);

    if (items[1].revents & ZMQ_POLLIN) {
      zmq::message_t term_msg;
      auto term_res = sub_socket.recv(
          term_msg, zmq::recv_flags::none); // 这里 poll 了，肯定要有事件
      if (!term_res.has_value()) {
        printf("sub_socket confusing\n");
        exit(-1);
      }
      break;
    }

    if (items[0].revents & ZMQ_POLLIN) {
      // 也许我们压根就不需要 zmq::message_t，直接用 buffer 就行
      auto recv_res =
          rep_socket.recv(zmq::mutable_buffer(&request, sizeof(Request)),
                          zmq::recv_flags::none);
      if (!recv_res.has_value() || recv_res->size != sizeof(Request)) {
        printf("recv failed\n");
        exit(-1);
      }

      // std::this_thread::sleep_for(std::chrono::milliseconds(3));
      fprintf(fp, "req_id = %d, which = %d, user_data = %d\n", request.req_id_,
              request.which_, request.user_data_);

      // 这玩意的 data 之前是零拷贝指定给 msg 的，所以这里可以直接用
      int which = request.which_;

      rep_socket.send(zmq::message_t(),
                      zmq::send_flags::none); // 空消息
      rep_sent++;

      push_socket.send(
          zmq::message_t(&which, sizeof(int)), // 有拷贝，因为我们无法保证 which
                                               // 在 push 完成前始终有效
          zmq::send_flags::none);
      push_sent++;
    }
  }

  fclose(fp);
  rep_socket.close();
  push_socket.close();
  sub_socket.close();
  context.close();

  std::cout << "Client " << mpi_rank << " Stats: REP Sent = " << rep_sent
            << ", PUSH Sent = " << push_sent << std::endl;
}

// g++ -o zmqlearn zmqlearn.cc -Wall -O2 -lzmq
// export LD_LIBRARY_PATH=/usr/local/lib64:$LD_LIBRARY_PATH
// mpirun -x LD_LIBRARY_PATH -np 4 ./build/zmqlearn
int main(int argc, char **argv) {
  srand(time(nullptr));
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);

  if (mpi_rank == 0) {
    server();
  } else {
    client();
  }

  MPI_Finalize();
  return 0;
}
