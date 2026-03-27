Ride-Sharing Dispatch System
A concurrent, C-based ride-sharing simulation using TCP Sockets and I/O Multiplexing (select). This system allows passengers to request rides and drivers to accept them, featuring real-time location tracking and robust error handling for disconnected peers.

Features：

1. Centralized Dispatch Server: Handles multiple concurrent clients using a non-blocking select() loop.

2. Interaction Based On Role: Distinct logic for Drivers and Passengers.

3. Real Time Tracking: Drivers can stream coordinates to matched passengers.

4. State Machine Management: Prevents race conditions by locking driver status during dispatch.

5. Fault Tolerance: Automatically notifies and resets clients if their matched peer disconnects unexpectedly.

Compilation
The project includes a Makefile for easy compilation.

Bash
# Compile all components (dispatch, driver, passenger)
make

# Clean build files
make clean


System Scenarios & Testing
To fully understand the system, run the following test cases in separate terminals. 

Scenario A: Full Workflow (One Driver, Multiple Passengers)
Goal: Verify sequential order processing and message relaying.

Setup: Launch the dispatch server, then login Driver_1.

Action: Login Passenger_A and Passenger_B.

Sequential Requests:

Passenger_A requests a ride.

Driver_1 accepts the request and completes the trip by entering a.

After the first trip is billed and completed, Passenger_B requests a ride.

Logic: The server correctly handles the message lifecycle: MSG_RIDE_REQUEST → MSG_DISPATCH_JOB → MSG_ACCEPT → MSG_BILL.

Observation: The system successfully cycles the driver back to an idle state after each completion, allowing for continuous sequential service.

Scenario B: Priority Testing (Static Dispatch)
Goal: Observe the server's driver selection strategy.

Procedure: Launch two drivers (Driver_1 and Driver_2). Login one passenger and request a ride.

Logic: The server performs a linear scan of the client array using a loop: for (int i = 0; i < MAX_CLIENTS; i++).

Observation: The driver who logged in first (occupying the lower array index) will always receive the dispatch first. Driver_2 remains idle until Driver_1 is either busy or disconnected.

Scenario C: Peer Disconnection
Goal: Verify system cleanup when a connection is lost during an active trip.

Procedure: Start a trip so the status is ON_TRIP.

Trigger: Force close the Driver terminal

Logic:

The server's select() function detects the closed socket.

The server identifies the matched peer_fd associated with the passenger.

The server sends a MSG_ERROR specifically to that passenger.

Observation: The passenger displays ERROR: Matched peer disconnected. Their status is immediately reset to IDLE, allowing them to request a new ride without restarting the application.

Scenario D: Race Condition and Resource Locking
Goal: Verify the system handles multiple simultaneous requests when driver resources are limited.

Setup: Login one driver and two passengers.

Conflict: Both passengers send a MSG_RIDE_REQUEST at nearly the same time.

Logic:

The first request processed by the server assigns the driver and changes their status to STATUS_ASSIGNED.

When the second request arrives, the find_idle_driver function scans the array but finds no drivers remaining in the STATUS_IDLE state.

Observation:

Passenger_A: Successfully matched with the driver.

Passenger_B: Immediately receives ERROR: No idle driver available.

Conclusion: This proves the STATUS_ASSIGNED flag acts as a logical lock, effectively preventing "double-booking" or race conditions.