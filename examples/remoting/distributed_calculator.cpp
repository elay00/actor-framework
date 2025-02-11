// This program is a distributed version of the math_actor example. Client and
// server use a stateless request/response protocol and the client is failure
// resilient by using a FIFO request queue. The client auto-reconnects and also
// allows for server reconfiguration.
//
// Run server at port 4242:
// - distributed_calculator -s -p 4242
//
// Run client at the same host:
// - distributed_calculator -c -p 4242

#include <array>
#include <cassert>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "caf/all.hpp"
#include "caf/io/all.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::string;

using namespace caf;

namespace {

constexpr auto task_timeout = std::chrono::seconds(10);

// our "service"
behavior calculator_fun() {
  return {
    [](add_atom, int a, int b) { return a + b; },
    [](sub_atom, int a, int b) { return a - b; },
  };
}

// State transition of the client for connecting to the server:
//
//                    +-------------+
//                    |    init     |
//                    +-------------+
//                           |
//                           V
//                    +-------------+
//                    | unconnected |<------------------+
//                    +-------------+                   |
//                           |                          |
//                           | {connect Host Port}      |
//                           |                          |
//                           V                          |
//                    +-------------+  {error}          |
//    +-------------->| connecting  |-------------------+
//    |               +-------------+                   |
//    |                      |                          |
//    |                      | {ok, Calculator}         |
//    |{connect Host Port}   |                          |
//    |                      V                          |
//    |               +-------------+   {DOWN server}   |
//    +---------------|   running   |-------------------+
//                    +-------------+

namespace client {

// a simple calculator task: operation + operands
struct task {
  std::variant<add_atom, sub_atom> op;
  int lhs;
  int rhs;
};

// the client queues pending tasks
struct state {
  strong_actor_ptr current_server;
  std::vector<task> tasks;
};

// prototype definition for unconnected state
behavior unconnected(stateful_actor<state>*);

// prototype definition for transition to `connecting` with Host and Port
void connecting(stateful_actor<state>*, const std::string& host, uint16_t port);

// prototype definition for transition to `running` with Calculator
behavior running(stateful_actor<state>*, const actor& calculator);

// starting point of our FSM
behavior init(stateful_actor<state>* self) {
  // transition to `unconnected` on server failure
  self->set_down_handler([=](const down_msg& dm) {
    if (dm.source == self->state.current_server) {
      aout(self) << "*** lost connection to server" << endl;
      self->state.current_server = nullptr;
      self->become(unconnected(self));
    }
  });
  return unconnected(self);
}

behavior unconnected(stateful_actor<state>* self) {
  return {
    [=](add_atom op, int x, int y) {
      self->state.tasks.emplace_back(task{op, x, y});
    },
    [=](sub_atom op, int x, int y) {
      self->state.tasks.emplace_back(task{op, x, y});
    },
    [=](connect_atom, const std::string& host, uint16_t port) {
      connecting(self, host, port);
    },
  };
}

void connecting(stateful_actor<state>* self, const std::string& host,
                uint16_t port) {
  // make sure we are not pointing to an old server
  self->state.current_server = nullptr;
  // use request().await() to suspend regular behavior until MM responded
  auto mm = self->system().middleman().actor_handle();
  self->request(mm, infinite, connect_atom_v, host, port)
    .await(
      [=](const node_id&, strong_actor_ptr serv,
          const std::set<std::string>& ifs) {
        if (!serv) {
          aout(self) << R"(*** no server found at ")" << host << R"(":)" << port
                     << endl;
          return;
        }
        if (!ifs.empty()) {
          aout(self) << R"(*** typed actor found at ")" << host << R"(":)"
                     << port << ", but expected an untyped actor " << endl;
          return;
        }
        aout(self) << "*** successfully connected to server" << endl;
        self->state.current_server = serv;
        auto hdl = actor_cast<actor>(serv);
        self->monitor(hdl);
        self->become(running(self, hdl));
      },
      [=](const error& err) {
        aout(self) << R"(*** cannot connect to ")" << host << R"(":)" << port
                   << " => " << to_string(err) << endl;
        self->become(unconnected(self));
      });
}

// prototype definition for transition to `running` with Calculator
behavior running(stateful_actor<state>* self, const actor& calculator) {
  auto send_task = [=](auto op, int x, int y) {
    self->request(calculator, task_timeout, op, x, y)
      .then(
        [=](int result) {
          const char* op_str;
          if constexpr (std::is_same<add_atom, decltype(op)>::value)
            op_str = " + ";
          else
            op_str = " - ";
          aout(self) << x << op_str << y << " = " << result << endl;
        },
        [=](const error&) {
          // simply try again by enqueueing the task to the mailbox again
          self->send(self, op, x, y);
        });
  };
  for (auto& x : self->state.tasks) {
    auto f = [&](auto op) { send_task(op, x.lhs, x.rhs); };
    std::visit(f, x.op);
  }
  self->state.tasks.clear();
  return {
    [=](add_atom op, int x, int y) { send_task(op, x, y); },
    [=](sub_atom op, int x, int y) { send_task(op, x, y); },
    [=](connect_atom, const std::string& host, uint16_t port) {
      connecting(self, host, port);
    },
  };
}

} // namespace client

// removes leading and trailing whitespaces
string trim(std::string s) {
  auto not_space = [](char c) { return isspace(c) == 0; };
  // trim left
  s.erase(s.begin(), find_if(s.begin(), s.end(), not_space));
  // trim right
  s.erase(find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// tries to convert `str` to an int
std::optional<int> toint(const string& str) {
  char* end;
  auto result = static_cast<int>(strtol(str.c_str(), &end, 10));
  if (end == str.c_str() + str.size())
    return result;
  return std::nullopt;
}

// --(rst-config-begin)--
class config : public actor_system_config {
public:
  uint16_t port = 0;
  std::string host = "localhost";
  bool server_mode = false;

  config() {
    opt_group{custom_options_, "global"}
      .add(port, "port,p", "set port")
      .add(host, "host,H", "set host (ignored in server mode)")
      .add(server_mode, "server-mode,s", "enable server mode");
  }
};
// --(rst-config-end)--

void client_repl(actor_system& system, const config& cfg) {
  // keeps track of requests and tries to reconnect on server failures
  auto usage = [] {
    cout << "Usage:" << endl
         << "  quit                  : terminates the program" << endl
         << "  connect <host> <port> : connects to a remote actor" << endl
         << "  <x> + <y>             : adds two integers" << endl
         << "  <x> - <y>             : subtracts two integers" << endl
         << endl;
  };
  usage();
  bool done = false;
  auto client = system.spawn(client::init);
  if (!cfg.host.empty() && cfg.port > 0)
    anon_send(client, connect_atom_v, cfg.host, cfg.port);
  else
    cout << "*** no server received via config, "
         << R"(please use "connect <host> <port>" before using the calculator)"
         << endl;
  // defining the handler outside the loop is more efficient as it avoids
  // re-creating the same object over and over again
  message_handler eval{
    [&](const string& cmd) {
      if (cmd != "quit")
        return;
      anon_send_exit(client, exit_reason::user_shutdown);
      done = true;
    },
    [&](string& arg0, string& arg1, string& arg2) {
      if (arg0 == "connect") {
        char* end = nullptr;
        auto lport = strtoul(arg2.c_str(), &end, 10);
        if (end != arg2.c_str() + arg2.size())
          cout << R"(")" << arg2 << R"(" is not an unsigned integer)" << endl;
        else if (lport > std::numeric_limits<uint16_t>::max())
          cout << R"(")" << arg2 << R"(" > )"
               << std::numeric_limits<uint16_t>::max() << endl;
        else
          anon_send(client, connect_atom_v, std::move(arg1),
                    static_cast<uint16_t>(lport));
      } else {
        auto x = toint(arg0);
        auto y = toint(arg2);
        if (x && y) {
          if (arg1 == "+")
            anon_send(client, add_atom_v, *x, *y);
          else if (arg1 == "-")
            anon_send(client, sub_atom_v, *x, *y);
        }
      }
    }};
  // read next line, split it, and feed to the eval handler
  string line;
  while (!done && std::getline(std::cin, line)) {
    line = trim(std::move(line)); // ignore leading and trailing whitespaces
    std::vector<string> words;
    split(words, line, is_any_of(" "), token_compress_on);
    auto msg = message_builder(words.begin(), words.end()).move_to_message();
    if (!eval(msg))
      usage();
  }
}

void run_server(actor_system& system, const config& cfg) {
  auto calc = system.spawn(calculator_fun);
  // try to publish math actor at given port
  cout << "*** try publish at port " << cfg.port << endl;
  auto expected_port = io::publish(calc, cfg.port);
  if (!expected_port) {
    std::cerr << "*** publish failed: " << to_string(expected_port.error())
              << endl;
    return;
  }
  cout << "*** server successfully published at port " << *expected_port << endl
       << "*** press [enter] to quit" << endl;
  string dummy;
  std::getline(std::cin, dummy);
  cout << "... cya" << endl;
  anon_send_exit(calc, exit_reason::user_shutdown);
}

void caf_main(actor_system& system, const config& cfg) {
  auto f = cfg.server_mode ? run_server : client_repl;
  f(system, cfg);
}

} // namespace

CAF_MAIN(io::middleman)
