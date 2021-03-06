// Add the following line to enter the (verbose) debug mode.
// Be warned that it will most likely break any connected backends
// as they will receive unexpected messages through the serial interface
// #define SERIAL_COMMANDS_DEBUG

#include <SimpleTimer.h>    // https://github.com/marcelloromani/Arduino-SimpleTimer
#include <SerialCommands.h> // https://github.com/ppedro74/Arduino-SerialCommands/
#include <EEPROM.h>
#include "AutoPID.h" // AutoPID Arduino Library Module, modified to use integers instead of double values
#include "EEPROMAnything.h"
char serial_command_buffer_[64];

// '\n' is the line termination command for this serial interface
SerialCommands serial_commands_(&Serial, serial_command_buffer_, sizeof(serial_command_buffer_), "\n", " ");

SimpleTimer timer;
//------------------------------------------------------------------------------

//------------------//
// GLOBAL VARIABLES //
//------------------//
char fw_version[4] = "0.2";
int target_temp;
int pidP;
int pidI;
int pidD;
int pidB;
int minrpm;
int maxrpm;
int rpm;
int ontime;
int offtime;
int restime;
int sensor;                     //which sensor to use for the PID controller (0...2; 3 means external temperature)
int filter_threshold;           //value in mBar
int sensor_temps[4];  //values in °C
int pressure;                   //value in mBar of pressure difference
int external_reference;
int mode;                       //0 = gpu, 1 = asic
int commit_frequency;
const int MAX_MINERS = 120;
int miners[MAX_MINERS];         //255 = disabled/not present, 0 = off, 1 = on
bool filter_ok = true;
bool status = true;

int timer_id;
//------------------------------------------------------------------------------

// Can be removed, only used for PWM testing
int blink_row = 0;

#define OUTPUT_MIN 0
#define OUTPUT_MAX 255
int rpm_output = 0;
AutoPID myPID(&sensor_temps[sensor], &target_temp, &rpm_output, OUTPUT_MIN, OUTPUT_MAX, pidP, pidI, pidD);

// Starting at -1, as the first call to addr() will advance the marker
int addr_marker = -1;
int addr(int offset) { addr_marker += offset; return addr_marker; }
int addr() { return addr(1); }

//------------------//
// EEPROM ADDRESSES //
//------------------//
// 1 byte values
const int ADDR_INIT_MARKER = addr();
const int ADDR_TARGET_TEMP = addr();
const int ADDR_PIDP = addr();
const int ADDR_PIDI = addr();
const int ADDR_PIDD = addr();
const int ADDR_PIDB = addr();
const int ADDR_MINRPM = addr();
const int ADDR_MAXRPM = addr();
const int ADDR_EXTERNAL = addr();
const int ADDR_MODE = addr();
const int ADDR_FREQUENCY = addr();
// 4 byte values
const int ADDR_SENSOR = addr();
const int ADDR_THRESHOLD = addr(sizeof(int));
const int ADDR_ONTIME = addr(sizeof(int));
const int ADDR_OFFTIME = addr(sizeof(int));
const int ADDR_RESTIME = addr(sizeof(int));
// > 4 byte values
const int ADDR_TEMPS = addr(sizeof(int)); // int[4] size = 16 byte
const int ADDR_VERSION = addr(sizeof(sensor_temps)); // String size = 12 byte
const int ADDR_MINERS = addr(sizeof(fw_version)); // int[120] size = 480 byte
const int ADDR_END = 1024;
//------------------//
// HELPER FUNCTIONS //
//------------------//
void return_OK(SerialCommands* sender) { echo(sender, "OK"); }
void return_OK(SerialCommands* sender, String msg) { echo(sender, "OK - " + msg); }
void return_ERR(SerialCommands* sender) { echo(sender, "ERR"); }
void return_ERR(SerialCommands* sender, String msg) { echo(sender, "ERR - " + msg); }

/*
 * This is the default handler, and gets called when no other command matches.
 */
void cmd_unrecognized(SerialCommands* sender, const char* cmd) {
  sender->GetSerial()->print("Unrecognized command [");
  sender->GetSerial()->print(cmd);
  sender->GetSerial()->println("]");
}

/*
 * Checks whether the param is NULL
 * Returns true if a valid number is given.
 */
bool check_param(SerialCommands* sender, char* param) {
  if (param == NULL) {
    return_ERR(sender, "no argument given");
    return false;
  }
  return true;
}

/*
 * Checks the param (see check_param) internally and sets the resulting int
 * as the referenced variable value.
 */
void set_arg(SerialCommands* sender, int& injectedVar) {
  char* temp = sender->Next();
  if (!check_param(sender, temp)) {
    return;
  }
  injectedVar = atoi(temp);
}

/*
 * Theoretically this clears the EEPROM, but I recommend against executing this because it is
 * incredibly slow and it hurts the lifetime of the flash memory of the nucleo board (as it has no real EEPROM).
 * If you ever want to reset the EEPROM for testing, I recommend to !commit and then to reset the board after
 * a few seconds. The mismatching CRC value will cause the program to reinitialize the configuration and write
 * it to the EEPROM.
 */
void clear_EEPROM() {
  for (int i = 0 ; i < EEPROM.length() ; ++i) {
    EEPROM.write(i, 0);
    Serial.println(i);
  }
}

bool EEPROM_is_empty() {
  unsigned long saved = EEPROM_readAnything(ADDR_END);
  unsigned long calculated = EEPROM_crc();

  #ifdef SERIAL_COMMANDS_DEBUG
  Serial.print("saved: ");
  Serial.print(saved);
  Serial.print(", calc: ");
  Serial.print(calculated);
  Serial.print(", EEPROM clean: ");
  Serial.println(!(saved==calculated));
  #endif

  return !(saved == calculated);
}

/*
 * Performs a basic CRC check up until the ADDR_END (currently 1kB).
 * source: https://www.arduino.cc/en/Tutorial/EEPROMCrc 
 */
unsigned long EEPROM_crc(void) {

  const unsigned long crc_table[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
  };

  unsigned long crc = ~0L;
  for (int index = 0 ; index < ADDR_END; ++index) {
    crc = crc_table[(crc ^ EEPROM[index]) & 0x0f] ^ (crc >> 4);
    crc = crc_table[(crc ^ (EEPROM[index] >> 4)) & 0x0f] ^ (crc >> 4);
    crc = ~crc;
  }
  return crc;
}

void save_crc(void) {
  EEPROM_writeAnything(ADDR_END, EEPROM_crc());
}

/* Prints the variable to the senders serial interface */
void echo(SerialCommands* sender, int var) { sender->GetSerial()->println(var); }
void echo(SerialCommands* sender, String var) { sender->GetSerial()->println(var); }

//------------------------------------------------------------------------------

//----------------------------//
// USER INTERACTION FUNCTIONS //
//----------------------------//

void get_fw_version(SerialCommands* sender) { echo(sender, fw_version); } //?fw
void get_pidp(SerialCommands* sender) { echo(sender, pidP); } //?pidp
void get_pidi(SerialCommands* sender) { echo(sender, pidI); } //?pidi
void get_pidd(SerialCommands* sender) { echo(sender, pidD); } //?pidd
void get_pidb(SerialCommands* sender) { echo(sender, pidB); } //?pidb
void get_target_temp(SerialCommands* sender) { echo(sender, target_temp); } //?targettemp
void get_pressure(SerialCommands* sender) { echo(sender, pressure); } //?pressure
void get_pressure_threshold(SerialCommands* sender) { echo(sender, filter_threshold); } //?threshold
void get_filter(SerialCommands* sender) { echo(sender, filter_ok); } //?filter
void get_sensor(SerialCommands* sender) { echo(sender, sensor); } //?sensor
void get_minrpm(SerialCommands* sender) { echo(sender, minrpm); } //?minrpm
void get_maxrpm(SerialCommands* sender) { echo(sender, maxrpm); } //?maxrpm
void get_rpm(SerialCommands* sender) { echo(sender, rpm); } //?rpm
void get_ontime(SerialCommands* sender) { echo(sender, ontime); } //?ontime
void get_offtime(SerialCommands* sender) { echo(sender, offtime); } //?offtime
void get_restime(SerialCommands* sender) { echo(sender, restime); } //?restime
void get_external_reference(SerialCommands* sender) { echo(sender, external_reference); } //?external
void get_mode(SerialCommands* sender) { echo(sender, mode); } //?mode
void get_crc(SerialCommands* sender) { echo(sender, EEPROM_crc()); } //?ctc
void get_maxminers(SerialCommands* sender) { echo(sender, MAX_MINERS); } //?maxminers
void get_frequency(SerialCommands* sender) { echo(sender, commit_frequency); } //?frequency
//?temps
void get_temps(SerialCommands* sender) {
  String temp = "";
  for (int i = 0; i < sizeof(sensor_temps)/sizeof(int); ++i) {
    temp += i;
    temp += ":";
    temp += sensor_temps[i];
    temp += ", ";
  }
  temp.remove(temp.length() - 2); //Removing the last ,_
  echo(sender, temp);
}

//?miner <id>
void get_miner(SerialCommands* sender) {
  String raw_id = String(sender->Next());
  if (raw_id == "") {
    return_ERR(sender, "no argument given");
    return;
  }
  int id = raw_id.toInt();
  if (id < 0 or id >= MAX_MINERS) {
    return_ERR(sender, "invalid ID given");
    return;
  }
  echo(sender, miners[id]);
}
//?miners
/**
 * Gets states of all miners as a ',_' separated string. This is very useful to increase the performance in case you need more
 * than a few miner states.
 */
void get_miners(SerialCommands* sender) {
  String resp = "";
  for (int i = 0; i < MAX_MINERS; ++i) {
    resp += miners[i];
    resp += ", "; 
  }
  resp.remove(resp.length() - 2); //Removing the last ,_
  echo(sender, resp);
}
// ?all
/**
 * Returns all sensor values and configurations.
 */
void get_all(SerialCommands*sender) {
  echo(sender, String("fw: " + String(fw_version)));
  echo(sender, String("target_temp: " + String(target_temp)));
  echo(sender, String("pidp: " + String(pidP) + ", pidi : " + String(pidI) + ", pidd: " + String(pidI) + ", pidb: " + String(pidB)));
  echo(sender, String("minrpm: " + String(minrpm) + ", maxrpm: " + String(maxrpm)));
  echo(sender, String("mode: " + String(mode) + ", ontime: " + String(ontime) + ", offtime: " + String(offtime) + ", restime: " + String(restime)));
  String temp = "";
  for (int i = 0; i < sizeof(sensor_temps)/sizeof(int); ++i) {
    temp += i;
    temp += ":";
    temp += sensor_temps[i];
    temp += ", ";
  }
  temp.remove(temp.length() - 2); //Removing the last ,_
  echo(sender, String("sensor_id: " + String(sensor) + ", sensor_temps: " + temp + ", external ref.: " + String(external_reference)));
  echo(sender, String("filter_threshold: " + String(filter_threshold) + ", pressure: " + String(pressure) + ", filter_status: " + String(filter_ok)));
}

void set_target_temp(SerialCommands* sender) { set_arg(sender, target_temp); setup_PID(); } //!targettemp <int>
void set_sensor(SerialCommands* sender) { set_arg(sender, sensor); setup_PID(); } //!sensor <int>
void set_external_reference(SerialCommands* sender) { set_arg(sender, external_reference); } //!external <int>
void set_pressure_threshold(SerialCommands* sender) { set_arg(sender, filter_threshold); } //!threshold <int>
void set_minrpm(SerialCommands* sender) { set_arg(sender, minrpm); setup_PID(); } //!minrpm <int>
void set_maxrpm(SerialCommands* sender) { set_arg(sender, maxrpm); setup_PID(); } //!maxrpm <int>
void set_mode(SerialCommands* sender) { set_arg(sender, mode); } //!mode <int>
void set_ontime(SerialCommands* sender) { set_arg(sender, ontime); } //!ontime <int>
void set_offtime(SerialCommands* sender) { set_arg(sender, offtime); } //!offtime <int>
void set_restime(SerialCommands* sender) { set_arg(sender, restime); } //!restime <int>
void set_pidp(SerialCommands* sender) { set_arg(sender, pidP); setup_PID(); } //!pidp <int>
void set_pidi(SerialCommands* sender) { set_arg(sender, pidI); setup_PID(); } //!pidi <int>
void set_pidd(SerialCommands* sender) { set_arg(sender, pidD); setup_PID(); } //!pidd <int>
void set_pidb(SerialCommands* sender) { set_arg(sender, pidB); setup_PID(); } //!pidb <int>
void set_frequency(SerialCommands* sender) { 
  int old_frequency = commit_frequency;
  set_arg(sender, commit_frequency); 
  // if the frequency value has been updated, remove the old timer and set a new one.
  if (old_frequency != commit_frequency) {
    timer.deleteTimer(timer_id);
    timer_id = timer.setInterval(1000 * 60 * 60 * commit_frequency, auto_commit);
  }
} //!frequency <int>

//!miner <id> <action>
void set_miner(SerialCommands* sender) {
  String raw_id = String(sender->Next());
  if (raw_id == "") {
    return_ERR(sender, "no argument given");
    return;
  }
  int id = raw_id.toInt();
  int action = atoi(sender->Next());

  if (action < -1 and action > 1) {
    return_ERR(sender, "invalid argument given");
    return;
  }
  // 1 = on or register, 0 = off, 255 = deregister
  miners[id] = action;
}

void _commit() {
  // Lighting up all onboard LEDs to show the commit process
  // This can be safely removed in the final version
  int state1 = digitalRead(PB0);
  int state2 = digitalRead(PB7);
  int state3 = digitalRead(PB14);
  digitalWrite(PB0, HIGH);
  digitalWrite(PB7, HIGH);
  digitalWrite(PB14, HIGH);
  
  EEPROM_write(ADDR_TARGET_TEMP, target_temp);
  EEPROM_write(ADDR_PIDP, pidP);
  EEPROM_write(ADDR_PIDI, pidI);
  EEPROM_write(ADDR_PIDD, pidD);
  EEPROM_write(ADDR_PIDB, pidB);
  EEPROM_write(ADDR_MINRPM, minrpm);
  EEPROM_write(ADDR_MAXRPM, maxrpm);
  EEPROM_write(ADDR_SENSOR, sensor);
  EEPROM_write(ADDR_FREQUENCY, commit_frequency);
  EEPROM_write(ADDR_MODE, mode);
  EEPROM_write(ADDR_EXTERNAL, external_reference);
  for (int id = 0; id < MAX_MINERS; ++id) {
    EEPROM_write(ADDR_MINERS + id, miners[id]);
  }

  EEPROM_writeAnything(ADDR_THRESHOLD, filter_threshold);
  EEPROM_writeAnything(ADDR_ONTIME, ontime);
  EEPROM_writeAnything(ADDR_OFFTIME, offtime);
  EEPROM_writeAnything(ADDR_RESTIME, restime);

  save_crc();

  digitalWrite(PB0, state1);
  digitalWrite(PB7, state2);
  digitalWrite(PB14, state3);
}

//!commit
/*
 * In order to persist changes across restarts, the configuration needs to be written to the EEPROM.
 * As saving them with every value would take up too much time, the write process is centralized in
 * this function/command. For more information regarding this design decision view the following
 * stackoverflow question (asked & answered by me):
 * 
 * https://stackoverflow.com/questions/51302313/reading-serial-commands-takes-too-much-time
 */
void commit_changes(SerialCommands* sender) {
  _commit();

  // DO NOT REMOVE this echo, it acts as a handle for the backend to notice the end of
  // writing process.
  echo(sender, "Changes committed.");
}

//------------------------------------------------------------------------------

//-------------------//
// SERVICE FUNCTIONS //
//-------------------//

/*
 * TODO: @Georg
 * So unfortunately I've never found the time to appropriately test the PID routine.
 * You should test it once you got access to the real sensors.
 */
void setup_PID(void) {
  // Converting relative to absolute 1 byte values
  int minrpm_rel = (minrpm * 255)/100;
  int maxrpm_rel = (maxrpm * 255)/100;
  myPID.setBangBang(minrpm_rel, maxrpm_rel);
  myPID.setGains(pidP, pidI, pidD);
  myPID.setTimeStep(2000);
}

void check_filter_status(void) {
  filter_ok = pressure < filter_threshold;
}

/**
 * Initializes basic values. Is currently called in case the EEPROM is corrupted (crc does not match).
 */
void init_values(void) {
  target_temp = EEPROM_write(ADDR_TARGET_TEMP, 35);
  pidP = EEPROM_write(ADDR_PIDP, 6);
  pidI = EEPROM_write(ADDR_PIDI, 2);
  pidD = EEPROM_write(ADDR_PIDD, 0);
  pidB = EEPROM_write(ADDR_PIDB, 2);
  minrpm = EEPROM_write(ADDR_MINRPM, 5);
  maxrpm = EEPROM_write(ADDR_MAXRPM, 80);
  sensor = EEPROM_write(ADDR_SENSOR, 1);
  mode = EEPROM_write(ADDR_MODE, 0);
  commit_frequency = EEPROM_write(ADDR_FREQUENCY, 12);
  external_reference = EEPROM_write(ADDR_EXTERNAL, 55);
  for (int i = 0; i < MAX_MINERS; ++i) {
    miners[i] = 1;
    EEPROM_write(ADDR_MINERS + i, miners[i]);
  }
  
  filter_threshold = EEPROM_writeAnything(ADDR_THRESHOLD, 1400);
  ontime = EEPROM_writeAnything(ADDR_ONTIME, 100);
  offtime = EEPROM_writeAnything(ADDR_OFFTIME, 85);
  restime = EEPROM_writeAnything(ADDR_RESTIME, 125);
}

void load_values(void) {
  target_temp = EEPROM_read(ADDR_TARGET_TEMP);
  pidP = EEPROM_read(ADDR_PIDP);
  pidI = EEPROM_read(ADDR_PIDI);
  pidD = EEPROM_read(ADDR_PIDD);
  pidB = EEPROM_read(ADDR_PIDB);
  minrpm = EEPROM_read(ADDR_MINRPM);
  maxrpm = EEPROM_read(ADDR_MAXRPM);
  sensor = EEPROM_read(ADDR_SENSOR);
  mode = EEPROM_read(ADDR_MODE);
  external_reference = EEPROM_read(ADDR_EXTERNAL);
  commit_frequency = EEPROM_read(ADDR_FREQUENCY);
  for (int i = 0; i < MAX_MINERS; ++i) {
    miners[i] = EEPROM_read(ADDR_MINERS + i);
  }
  ontime = EEPROM_readAnything(ADDR_ONTIME);
  offtime = EEPROM_readAnything(ADDR_OFFTIME);
  restime = EEPROM_readAnything(ADDR_RESTIME);
  filter_threshold = EEPROM_readAnything(ADDR_THRESHOLD);
}

/**
 * TODO @georg
 * Update all sensor values in here.
 */
void update_sensors(void) {
  rpm = random(minrpm + 10, maxrpm - 10);
  pressure = random(1200, 1500);
  for (int i = 0; i < sizeof(sensor_temps)/sizeof(int); ++i) {
    sensor_temps[i] = random(25, 30);
  }
}

//------------------------------------------------------------------------------

//-----------------//
// TIMER FUNCTIONS //
//-----------------//
void main_timer() {
  update_sensors();

  // blinking green status LED
  digitalWrite(PB0, status);
  status = !status;

  // flashing blue LED if filter needs cleaning
  check_filter_status();
  digitalWrite(PB7, !filter_ok);

  if (blink_row == 0) {
    analogWrite(D6, 0);
    analogWrite(D5, 255);
    analogWrite(D3, 255);
    blink_row++;
  } else if (blink_row == 1) {
    analogWrite(D6, 255);
    analogWrite(D5, 0);
    analogWrite(D3, 255);
    blink_row++;
  } else {
    analogWrite(D6, 255);
    analogWrite(D5, 255);
    analogWrite(D3, 0);
    blink_row = 0;
  }

  myPID.run();

  #ifdef SERIAL_COMMANDS_DEBUG
  Serial.print("temperature: ");
  Serial.print(sensor_temps[sensor]);
  Serial.print(", target temp: ");
  Serial.print(target_temp);
  Serial.print(", output: ");
  Serial.print(rpm_output - 128 + pidB); // this is the final PID value for the rpm
  Serial.print(", Kp: ");
  Serial.print(pidP);
  Serial.print(", Ki: ");
  Serial.print(pidI);
  Serial.print(", Kd: ");
  Serial.println(pidD);
  #endif
}

void auto_commit() {
  _commit();
}

//------------------------------------------------------------------------------

//-----------------//
// SERIAL COMMANDS //
//-----------------//
SerialCommand cmd_fw("?fw", get_fw_version);
SerialCommand cmd_getpidp("?pidp", get_pidp);
SerialCommand cmd_getpidi("?pidi", get_pidi);
SerialCommand cmd_getpidd("?pidd", get_pidd);
SerialCommand cmd_getpidb("?pidb", get_pidb);
SerialCommand cmd_gettemps("?temps", get_temps);
SerialCommand cmd_gettargettemp("?targettemp", get_target_temp);
SerialCommand cmd_getpress("?pressure", get_pressure);
SerialCommand cmd_getpressthreshold("?threshold", get_pressure_threshold);
SerialCommand cmd_getfilter("?filter", get_filter);
SerialCommand cmd_getsensor("?sensor", get_sensor);
SerialCommand cmd_getminrpm("?minrpm", get_minrpm);
SerialCommand cmd_getmaxrpm("?maxrpm", get_maxrpm);
SerialCommand cmd_getrpm("?rpm", get_rpm);
SerialCommand cmd_getontime("?ontime", get_ontime);
SerialCommand cmd_getofftime("?offtime", get_offtime);
SerialCommand cmd_getrestime("?restime", get_restime);
SerialCommand cmd_getexternal("?external", get_external_reference);
SerialCommand cmd_getmode("?mode", get_mode);
SerialCommand cmd_getminer("?miner", get_miner);
SerialCommand cmd_getminers("?miners", get_miners);
SerialCommand cmd_getall("?all", get_all);
SerialCommand cmd_getcrc("?crc", get_crc);
SerialCommand cmd_getmaxminers("?maxminers", get_maxminers);
SerialCommand cmd_getfrequency("?frequency", get_frequency);
SerialCommand cmd_settargettemp("!targettemp", set_target_temp);
SerialCommand cmd_setexternalreference("!external", set_external_reference);
SerialCommand cmd_setpressurethreshold("!threshold", set_pressure_threshold);
SerialCommand cmd_setmaxrpm("!maxrpm", set_maxrpm);
SerialCommand cmd_setminrpm("!minrpm", set_minrpm);
SerialCommand cmd_setmode("!mode", set_mode);
SerialCommand cmd_setontime("!ontime", set_ontime);
SerialCommand cmd_setofftime("!offtime", set_offtime);
SerialCommand cmd_setrestime("!restime", set_restime);
SerialCommand cmd_setpidp("!pidp", set_pidp);
SerialCommand cmd_setpidi("!pidi", set_pidi);
SerialCommand cmd_setpidd("!pidd", set_pidd);
SerialCommand cmd_setpidb("!pidb", set_pidb);
SerialCommand cmd_setminer("!miner", set_miner);
SerialCommand cmd_setsensor("!sensor", set_sensor);
SerialCommand cmd_setfrequency("!frequency", set_frequency);
SerialCommand cmd_commit("!commit", commit_changes);

void add_serial_commands() {
  serial_commands_.SetDefaultHandler(cmd_unrecognized);
  serial_commands_.AddCommand(&cmd_fw);
  serial_commands_.AddCommand(&cmd_getpidp);
  serial_commands_.AddCommand(&cmd_getpidi);
  serial_commands_.AddCommand(&cmd_getpidd);
  serial_commands_.AddCommand(&cmd_getpidb);
  serial_commands_.AddCommand(&cmd_gettemps);
  serial_commands_.AddCommand(&cmd_gettargettemp);
  serial_commands_.AddCommand(&cmd_getpress);
  serial_commands_.AddCommand(&cmd_getpressthreshold);
  serial_commands_.AddCommand(&cmd_getfilter);
  serial_commands_.AddCommand(&cmd_getsensor);
  serial_commands_.AddCommand(&cmd_getminrpm);
  serial_commands_.AddCommand(&cmd_getmaxrpm);
  serial_commands_.AddCommand(&cmd_getrpm);
  serial_commands_.AddCommand(&cmd_getontime);
  serial_commands_.AddCommand(&cmd_getofftime);
  serial_commands_.AddCommand(&cmd_getrestime);
  serial_commands_.AddCommand(&cmd_getexternal);
  serial_commands_.AddCommand(&cmd_getmode);
  serial_commands_.AddCommand(&cmd_getall);
  serial_commands_.AddCommand(&cmd_getcrc);
  serial_commands_.AddCommand(&cmd_getminer);
  serial_commands_.AddCommand(&cmd_getmaxminers);
  serial_commands_.AddCommand(&cmd_getfrequency);
  serial_commands_.AddCommand(&cmd_settargettemp);
  serial_commands_.AddCommand(&cmd_setexternalreference);
  serial_commands_.AddCommand(&cmd_setpressurethreshold);
  serial_commands_.AddCommand(&cmd_setminrpm);
  serial_commands_.AddCommand(&cmd_setmaxrpm);
  serial_commands_.AddCommand(&cmd_setmode);
  serial_commands_.AddCommand(&cmd_setontime);
  serial_commands_.AddCommand(&cmd_setofftime);
  serial_commands_.AddCommand(&cmd_setrestime);
  serial_commands_.AddCommand(&cmd_setpidp);
  serial_commands_.AddCommand(&cmd_setpidi);
  serial_commands_.AddCommand(&cmd_setpidd);
  serial_commands_.AddCommand(&cmd_setpidb);
  serial_commands_.AddCommand(&cmd_setminer);
  serial_commands_.AddCommand(&cmd_setsensor);
  serial_commands_.AddCommand(&cmd_commit);
  serial_commands_.AddCommand(&cmd_getminers);
  serial_commands_.AddCommand(&cmd_setfrequency);
}

//------------------------------------------------------------------------------

//--------------------------//
// SETUP CODE AND MAIN LOOP //
//--------------------------//
void setup() {
  pinMode(PB0, OUTPUT);
  pinMode(PB7, OUTPUT);
  pinMode(PB14, OUTPUT);
  
  Serial.begin(115200);

  if (EEPROM_is_empty()) {
    Serial.println("Initializing values. This may take a minute or two.");
    init_values();
    Serial.println("Initialization complete.");
  } else {
    load_values();
  }
  save_crc();
  timer.setInterval(500, main_timer);
  timer_id = timer.setInterval(1000 * 60 * 60 * commit_frequency, auto_commit);
  add_serial_commands();

  setup_PID();
}

void loop() {
  serial_commands_.ReadSerial();
  timer.run();
}

