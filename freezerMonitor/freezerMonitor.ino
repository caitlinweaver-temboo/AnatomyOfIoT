/*
 ###############################################################################
 #
 # Commercial Freezer Monitor
 #
 # https://temboo.com/blog/the-anatomy-of-a-commercial-freezer-monitor
 #
 # This application logs temperature data and freezer door status and sends SMS 
 # alerts to specified individuals when the temperature falls outside an expected 
 # range, or the door is open for longer than a specified amount of time. 
 #
 # A temperature sensor and a reed switch (door sensor) are attached to an MCU, 
 # which dispatches alerts when necessary. 
 #
 # This application uses the following services:
 #
 # Amazon Dynamo DB: https://temboo.com/library/Library/Amazon/DynamoDB/
 #
 # Twilio SMS Messages: https://temboo.com/library/Library/Twilio/SMSMessages/
 #
 ###############################################################################
*/

/*
 ###############################################################################
 #
 # Copyright 2016, Temboo Inc.
 # 
 # Licensed under the Apache License, Version 2.0 (the "License");
 # you may not use this file except in compliance with the License.
 # You may obtain a copy of the License at
 # 
 # http://www.apache.org/licenses/LICENSE-2.0
 # 
 # Unless required by applicable law or agreed to in writing,
 # software distributed under the License is distributed on an
 # "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 # either express or implied. See the License for the specific
 # language governing permissions and limitations under the License.
 #
 ###############################################################################
*/

#include <Bridge.h>
#include <Process.h>
#include <Temboo.h>
#include "TembooAccount.h"

// Set Choreo profile names
#define DYNAMODB_PROFILE_NAME "myDynamoDBProfile"
#define TWILIO_PROFILE_NAME "myTwilioProfile"

// Assign names to sensor pins
const int doorPin = 7;
const int tempPin = A0;

// Timing in milliseconds
const unsigned long minute = 60000;
const unsigned long second = 1000;

// For keeping track of the status of the door
bool doorClosed;
bool prevDoorClosed;
unsigned long openTime;
unsigned long openDuration;
unsigned long doorAlertLastSent;
bool sentDoorAlert = false;

// For keeping track of the last time events occured
unsigned long dataLastLogged;
unsigned long tempDataLastAlerted;

// Setup for getting date and time data
Process date;

// Set safe range of freezer temperature in degrees C
// Temperature values outside this range will trigger an alert
float maxTempC = -15.0; // set max threshold
float minTempC = -19.0; // set min threshold

// Set how long the door should be open (in milliseconds) before sending an alert
unsigned long openTooLong = 180000; // 180,000 = 3 min
// Set how often subsequent alerts should be sent if the freezer is still open after sending the first alert
unsigned long doorAlertDelay = 180000; // 180,000 = every 3 min
// Set how often data should be logged 
unsigned long rateToLogData = 3600000; // 3,600,000 ms = every 1 hr 
// Set minimum amount of time required to elapse between sending temperature alerts
unsigned long tempAlertDelay = 1800000; // 1,800,000 ms = every 30 min

/*
 * Setup code to run once at the beginning
 */
void setup() {
  Bridge.begin();
  Serial.begin(9600);
  // Setup for getting date and time data
  if (!date.running()) {
    date.begin("date");
    date.addParameter("+%m-%d-%Y %T");
    date.run();
  }
  // Initialize door status variables
  doorClosed, prevDoorClosed = digitalRead(doorPin);
  // Initialize data logging timing variables
  tempDataLastAlerted = tempAlertDelay;
  dataLastLogged = rateToLogData;
}

/*
 * Program code to loop continuously
 */
void loop() {
  checkDoor();
  checkTemperature();
  logData();
}

/*
 * Convert analog input of temperature sensor to degrees C
 */
float readTemperature() {
  // Average 100 readings of the temperature sensor
  uint32_t tempValue = 0;
  for (int i = 0; i < 100; i++) {
    tempValue += analogRead(tempPin);
  }
  tempValue /= 100;

  // Convert ADC counts to voltage in mv (10 bit ADC max voltage 5V)
  float tempMVolts = ((float)tempValue / 1024.0) * 5000.0;
  
  // Convert voltage to degrees C using parabolic equation from
  // http://www.ti.com/lit/ds/symlink/lmt84.pdf
  float tempC = ((5.506 - sqrt(30.316036 + (0.00704 * (870.6 - tempMVolts)))) / -0.00352) + 30;

  return tempC;
}

/*
 * Keep track of how long the door has been open and send an alert when
 * it has been open for too long.
 */
void checkDoor(){
  unsigned long currentTime = millis();
  doorClosed = digitalRead(doorPin);

  // Check whether the door just opened or closed
  if (doorClosed != prevDoorClosed) {
    sentDoorAlert = false;
    if (!doorClosed) {
      prevDoorClosed = doorClosed;
      openTime = millis();
      openDuration = 0;
    } else {
      prevDoorClosed = doorClosed;
    }
  } else if (!doorClosed) { 
    // If the door is still open, keep track of how long it has been open
    openDuration = currentTime - openTime;

    // Send an alert when the freezer door has been open for too long
    if (openDuration > openTooLong) {
      // Create a string to hold the alert message
      String openDurationMessage = "";
      // If this is the first alert for this instance of the door being open too long, 
      // or if enough time has passed since the last time a door alert was sent, send an alert
      if (!sentDoorAlert || (currentTime - doorAlertLastSent) >= (doorAlertDelay)) { 
        // Format alert message to display minutes and seconds as required
        if (openDuration > minute) {
          int secondsOpen = (openDuration % minute) / second;
          int minutesOpen = (openDuration - secondsOpen) / minute;
          if (secondsOpen == 0) {
            openDurationMessage = String(minutesOpen) + " min";
          } else {
            openDurationMessage = String(minutesOpen) + " min and " + String(secondsOpen) + " sec";
          }
        } else if (openDuration == minute) {
          openDurationMessage = "1 min";
        } else {
          openDurationMessage = String(openDuration / second) + " sec";
        }
        sendAlert("Alert: The freezer has been open for " + openDurationMessage + ".");
        doorAlertLastSent = millis();
        sentDoorAlert = true;
      }

    }
  }
}

/*
 * Send an alert if the frezer temperature is out of expected range
 */
void checkTemperature(){
  unsigned long currentTime = millis();

  // Get the current temperature
  float tempC = readTemperature();

  // If enough time has passed since the last alert, see if we need to send an alert
  if (currentTime - tempDataLastAlerted >= tempAlertDelay){
    // Check whether the freezer temperature is outside of the expected range
    if (tempC > maxTempC) {
      sendAlert("Alert: The current freezer temperature is " + String(tempC) + "ºC, which exceeds the maximum desired temperature of " + String(maxTempC) + "ºC.");
    } else if (tempC < minTempC) {
      sendAlert("Alert: The current freezer temperature is " + String(tempC) + "ºC, which is below the minimum desired temperature of " + String(minTempC) + "ºC.");
    }
    tempDataLastAlerted = millis();
  }
}

/*
 * Log timestamped temperature data at the specified rate
 */
void logData() {
  unsigned long currentTime = millis();

  // Log data if enough time has passed since the last time data was logged 
  if ((currentTime - dataLastLogged) >= (rateToLogData)){
    Serial.println("Running PutItem");

    TembooChoreo PutItemChoreo;

    // Invoke the Temboo client
    PutItemChoreo.begin();

    // Set Temboo account credentials
    PutItemChoreo.setAccountName(TEMBOO_ACCOUNT);
    PutItemChoreo.setAppKeyName(TEMBOO_APP_KEY_NAME);
    PutItemChoreo.setAppKey(TEMBOO_APP_KEY);
    
    // Set Profile to use for execution
    PutItemChoreo.setProfile(DYNAMODB_PROFILE_NAME);

    // Set database item input
    PutItemChoreo.addInput("Item", "{\n\"Timestamp\": {\"S\": \"" + createTimestamp() + "\"},\n\"Temperature\": {\"N\": \"" +  String(readTemperature()) + "\"},\n\"DoorClosed\": {\"BOOL\": \"" + String(doorClosed) + "\"}\n}");
    
    // Identify the Choreo to run
    PutItemChoreo.setChoreo("/Library/Amazon/DynamoDB/PutItem");
    
    // Run the Choreo; when results are available, print them to serial
    PutItemChoreo.run();
    
    while(PutItemChoreo.available()) {
      char c = PutItemChoreo.read();
      Serial.print(c);
    }
    PutItemChoreo.close();
    dataLastLogged = millis();
  }
}

/*
 * Send an SMS alert with the specified message
 */
void sendAlert(String message){
  Serial.println("Running SendSMS");
  
  TembooChoreo SendSMSChoreo;

  // Invoke the Temboo client
  SendSMSChoreo.begin();

  // Set Temboo account credentials
  SendSMSChoreo.setAccountName(TEMBOO_ACCOUNT);
  SendSMSChoreo.setAppKeyName(TEMBOO_APP_KEY_NAME);
  SendSMSChoreo.setAppKey(TEMBOO_APP_KEY);

  SendSMSChoreo.addInput("Body", message);
  
  // Set Profile to use for execution
  SendSMSChoreo.setProfile(TWILIO_PROFILE_NAME);
  
  // Identify the Choreo to run
  SendSMSChoreo.setChoreo("/Library/Twilio/SMSMessages/SendSMS");
  
  // Run the Choreo; when results are available, print them to serial
  SendSMSChoreo.run();
  
  while(SendSMSChoreo.available()) {
    char c = SendSMSChoreo.read();
    Serial.print(c);
  }
  SendSMSChoreo.close();
}

/*
 * Create a timestamp for the current time in the format MM-DD-YYYY 00:00:00
 */
String createTimestamp(){
  // Keep track of how long we've been trying to retrieve a date before we return an error
  unsigned long currentTime = millis();
  unsigned long retryTimer = 0;

  if (!date.running()) {
    date.begin("date");
    date.addParameter("+%m-%d-%Y %T");
    date.run();
  }

  // If there's no date result available, wait for a second to see if we can retrieve one
  while (date.available()<= 0){
    if (retryTimer < second) {
      delay(30);
      retryTimer = millis() - currentTime;
    } else {
      // Return a placeholder date string if the date process fails to return a result
      return "MM-DD-YYYY HH:MM:SS";
    } 
  }

  // If this code is running, then the date process has produced a result
  // Get the result of the date process
  String timeString = date.readString();
  // Remove newline char at end of timestamp
  timeString.trim();

  return timeString;
}
