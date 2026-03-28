Ride Sharing Dispatch System v2.0

A concurrent C based ride sharing simulation using TCP Sockets and IO Multiplexing select. This system simulates a real world hailing service with proximity based dispatching and distance based dynamic pricing.

New Logic Updates

Proximity Based Dispatch
Instead of a linear scan the server now calculates the Euclidean distance between all IDLE drivers and the passenger’s PICKUP location assigning the nearest available driver

Dynamic Pricing
Fares are no longer flat The system calculates the straight line distance from PICKUP to DROPOFF and charges a rate of 0.80 per unit distance

System Geography and Reference Map

The simulation operates on a 100 by 100 coordinate grid Passengers must enter locations from the following supported reference table

Location Name Airport
Coordinates 10.0 10.0
Description Bottom Left Transit Hub

Location Name Hotel
Coordinates 50.0 50.0
Description Center Business District

Location Name Station
Coordinates 20.0 80.0
Description Top Left Train Station

Location Name University
Coordinates 70.0 10.0
Description Bottom Right Academic Zone

Location Name Mall
Coordinates 90.0 90.0
Description Top Right Shopping Center

Driver Starting Positions
Every driver is assigned a randomized coordinate upon login to simulate a distributed fleet across the city

Core Features

Smart Dispatcher
Uses the Pythagorean theorem d equals sqrt delta x squared plus delta y squared to find the optimal driver

Dynamic Billing
Real time fare calculation based on a 0.8 per unit rate

Real Time Tracking
Drivers stream x y coordinates to passengers during the ON TRIP state

Persistent Loops
Both clients remain active after a trip completes allowing for immediate re requesting or re dispatching

Fault Tolerance
Peer disconnection detection with automatic state reset for the remaining party

Compilation

The project requires the math library lm for distance calculations

Compile all components dispatch driver passenger
make

Clean build files
make clean

System Scenarios and Testing

Scenario A Proximity Dispatching

Goal Verify the server selects the closest driver

Launch the Dispatch Server

Login Driver A randomly placed at 10 15 and Driver B randomly placed at 80 85

Login Passenger and enter Pickup Airport Coords 10 10

Observation The Server logs will show distance calculations for both drivers Driver A will receive the MSG DISPATCH JOB because they are significantly closer to the Airport

Scenario B Dynamic Pricing Logic

Goal Verify fare calculation accuracy

Passenger requests a ride from Airport 10 10 to Hotel 50 50

The trip distance is calculated as sqrt 40 squared plus 40 squared approximately 56.57

Upon arrival a the passenger receives a bill

Calculation 56.57 times 0.8 equals 45.26

Observation Passenger terminal displays BILL Trip Done Dist 56.57 Fare 45.26

Scenario C Post Trip State Reset

Goal Ensure system longevity without restarting

Complete a trip as described in Scenario B

Observation

Driver Returns to STATUS IDLE and waits for new dispatches

Passenger Returns to the New Ride Request prompt to enter a new destination

Technical Implementation Details

Server Uses a city map lookup table and sqrtf for geometry

Driver Uses srand time NULL xor getpid to ensure unique starting coordinates for multiple instances on the same host

Protocol All distance and fare data is encapsulated in the payload buffer of the ride msg t structure
