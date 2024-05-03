# Coroutine Generator

## Overview

The generator implementation is based on C++23 proposal P2502R2. It provides
asynchronous coroutine-based value generation with two variants:

- **Unbuffered**: Zero-copy, one suspension per element
- **Buffered**: Batches elements, amortizes suspension overhead

Unlike std::generator, operator() is a coroutine returning
std::optional<reference_type>.

## Object Relationships and Control Flow

The generator implementation uses awaiters to manage control flow between producer and consumer coroutines. Each awaiter has multiple entry/exit points:

### yield_awaiter lifecycle (4 entry/exit points):
1. Created by `co_yield value` → `promise.yield_value()` creates awaiter
2. `co_await yield_awaiter` → calls `await_suspend(producer_handle)`
3. `await_suspend()` branches:
   - Direct path: returns consumer_handle (symmetric transfer)
   - Scheduler path: schedules consumer, returns noop_coroutine()
4. `await_resume()` called:
   - By scheduler (after scheduler path)
   - After symmetric transfer back from consumer

### next_awaiter/call_awaiter lifecycle (4 entry/exit points):
1. Created by `co_await gen()` → `operator()` creates awaiter
2. `co_await call_awaiter` → calls `await_suspend(consumer_handle)`
3. `await_suspend()` branches:
   - Direct path: returns producer_handle (symmetric transfer)
   - Scheduler path: schedules producer, returns noop_coroutine()
4. `await_resume()` called:
   - By scheduler (after scheduler path)
   - After symmetric transfer back from producer

```mermaid
graph TB
    subgraph Consumer[" "]
        C_Exec[Consumer Executing]
        C_Suspended[Consumer Suspended]
        C_Resume[Consumer await_resume#40;#41;]

        C_Exec -->|co_await gen#40;#41;| CallAwaiter[Create call_awaiter]
        CallAwaiter -->|co_await| CA_Suspend[call_awaiter::await_suspend#40;#41;]

        CA_Suspend -->|!need_preempt#40;#41;<br/>return producer_handle| P_Exec
        CA_Suspend -->|need_preempt#40;#41;<br/>schedule#40;producer#41;<br/>return noop| C_Suspended

        P_Resume -->|symmetric transfer| C_Resume
        Sched -->|resume consumer| C_Resume
        C_Resume --> C_Exec
    end

    subgraph Producer[" "]
        P_Exec[Producer Executing]
        P_Suspended[Producer Suspended]
        P_Resume[Producer await_resume#40;#41;]

        P_Exec -->|co_yield value| YieldValue[promise.yield_value#40;#41;]
        YieldValue -->|create| YieldAwaiter[Create yield_awaiter]
        YieldAwaiter -->|co_await| YA_Suspend[yield_awaiter::await_suspend#40;#41;]

        YA_Suspend -->|!need_preempt#40;#41;<br/>return consumer_handle| C_Exec
        YA_Suspend -->|need_preempt#40;#41;<br/>schedule#40;consumer#41;<br/>return noop| P_Suspended

        C_Resume -->|symmetric transfer| P_Resume
        Sched -->|resume producer| P_Resume
        P_Resume --> P_Exec
    end

    Sched[Seastar Scheduler]

    style C_Exec fill:#e1f5ff
    style P_Exec fill:#ffe1f5
    style Sched fill:#fff5e1
    style CallAwaiter fill:#d0e0ff
    style YieldAwaiter fill:#ffd0e0
```

## Control Flow

Producer and consumer transfer control via symmetric transfer when
!need_preempt(). When need_preempt() returns true, the target coroutine
is scheduled via seastar::schedule() and the current coroutine suspends
to noop_coroutine().

The sequence diagrams below show the complete lifecycle of awaiters as
participants. Awaiters are shown with their creation and destruction
points documented via notes. The flow is split into two diagrams to show
each direction of control transfer.

### Consumer to Producer Flow

When consumer calls `co_await gen()`, control transfers to producer:

```mermaid
sequenceDiagram
    participant Consumer
    participant CallAwaiter as call_awaiter
    participant Producer
    participant Scheduler

    Note over Consumer: co_await gen()
    Consumer->>CallAwaiter: create call_awaiter
    Note over CallAwaiter: await_ready() = false
    Note over CallAwaiter: await_suspend(consumer)

    alt !need_preempt()
        Note over CallAwaiter: return producer_handle
        CallAwaiter-->>Producer: symmetric transfer
        Note over Consumer: consumer suspended
        Note over Producer: producer resumed
    else need_preempt()
        Note over CallAwaiter: return noop_coroutine()
        CallAwaiter->>Scheduler: schedule(producer)
        Note over Consumer: consumer suspended
        Scheduler->>Producer: resume producer
        Note over Producer: producer resumed
    end

    Note right of CallAwaiter: call_awaiter remains alive
```

### Producer to Consumer Flow

When producer executes `co_yield value`, control returns to consumer:

```mermaid
sequenceDiagram
    participant Producer
    participant YieldAwaiter as yield_awaiter
    participant Consumer
    participant CallAwaiter as call_awaiter
    participant Scheduler

    Note over Producer: co_yield value
    Producer->>YieldAwaiter: create yield_awaiter
    Note over YieldAwaiter: await_ready() = false
    Note over YieldAwaiter: await_suspend(producer)

    alt !need_preempt()
        Note over YieldAwaiter: return consumer_handle
        YieldAwaiter-->>Consumer: symmetric transfer
        Note over YieldAwaiter: destroyed
        Note over Producer: producer suspended
        Note over Consumer: consumer resumed
        Note over CallAwaiter: await_resume()
        CallAwaiter->>Consumer: return optional value
        Note over CallAwaiter: destroyed
        Note over Consumer: process value
    else need_preempt()
        Note over YieldAwaiter: return noop_coroutine()
        YieldAwaiter->>Scheduler: schedule(consumer)
        Note over YieldAwaiter: destroyed
        Note over Producer: producer suspended
        Scheduler->>Consumer: resume consumer
        Note over Consumer: consumer resumed
        Note over CallAwaiter: await_resume()
        CallAwaiter->>Consumer: return optional value
        Note over CallAwaiter: destroyed
        Note over Consumer: process value
    end
```

## Unbuffered Generator

### Characteristics

- Stores pointer to value in producer's stack frame
- No copies or moves
- One suspension per yielded element

### Usage

```cpp
generator<const T&> produce() {
    T value;
    co_yield value;  // Zero-copy: stores pointer only
}
```

Use when element moves are expensive or latency is critical.

## Buffered Generator

### Characteristics

- Accumulates elements in a container
- Suspends when buffer full or need_preempt() returns true
- Consumer drains buffer without suspensions

### Usage

```cpp
generator<const T&, T, circular_buffer_fixed_capacity<T, 128>> produce() {
    co_yield element;           // Individual elements
    co_yield std::span(data);   // Ranges
}
```

Use when throughput matters and element moves are cheap.

### Buffer Measurement

The buffered variant uses a customization point object to check buffer capacity:

```cpp
// Priority 1: Member function
struct MemoryBuffer {
    bool can_push_more() const {
        return memory_used < memory_limit;
    }
};

// Priority 2: ADL free function
namespace my_ns {
    bool can_push_more(const MyContainer& c);
}

// Priority 3: Default
return container.size() < container.capacity();
```

## Lifetime Guarantees

### Unbuffered

When co_yield evaluates an expression producing a glvalue, the object lives
until the coroutine resumes. The promise stores only a pointer.

### Buffered

Values are moved into the buffer and have independent lifetime.

## Exception Handling

Exceptions in the producer are caught by promise_type::unhandled_exception()
and stored. On the next consumer resumption, await_resume() rethrows the
exception.

## Template Parameters

### Single-parameter form

```cpp
generator<int>              // value_type=int, reference=int&&
generator<const string&>    // value_type=string, reference=const string&
```

### Two-parameter form

```cpp
generator<string_view, string>  // Return string_view, store string
```

Allows proxy reference pattern: producer yields string, consumer receives
string_view.

## Performance

Performance characteristics measured using benchmarks in `tests/perf/coroutine_perf.cc`.

### Benchmark Results

Each test generates 100 integers per iteration:

| test                                |     iters |           runtime |    allocs |     tasks |      inst |    cycles |
| -                                   |        -: |                -: |        -: |        -: |        -: |        -: |
| coroutine_test.unbuffered_generator |     10000 |  281.42ns ± 0.11% |     2.000 |     0.001 |   7189.52 |    1506.8 |
| coroutine_test.buffered_generator   |     10000 |  412.33ns ± 0.31% |     3.000 |     0.002 |  11317.70 |    2207.3 |

**Unbuffered generator:**
- One suspension per element (100 suspensions per iteration)
- Zero-copy: stores pointer to value in producer's stack frame
- Lower instruction count, predictable latency

**Buffered generator:**
- Amortized suspensions (~6-7 suspensions per iteration with buffer size 16)
- Moves elements into std::vector buffer
- Higher instruction count due to buffering overhead

### Choosing Between Variants

The choice depends on multiple factors:

**Use unbuffered when:**
- Element moves are expensive (large objects, non-trivial move constructors)
- Latency is critical (need first element ASAP)
- Memory pressure is a concern (no buffering overhead)
- Elements are naturally references to existing data

**Use buffered when:**
- Throughput matters more than latency
- Elements are cheap to move (integers, small PODs)
- Producer can generate elements in batches
- Using a fixed-capacity container (avoids heap allocations)

**Note:** The buffered variant's performance advantage becomes more pronounced with:
- Larger element counts (more amortization)
- Fixed-capacity containers like `circular_buffer_fixed_capacity` (no allocations)
- Higher element generation cost in the producer
- Natural batching in the data source
