#include <soc/gpio_reg.h>
#include <Wire.h>
#include <MCP7940.h>

//#define DEBUG
#ifdef DEBUG
  #define DEBUG_BEGIN(x)        Serial.begin(x)
  #define DEBUG_PRINT(x)        Serial.print(x)
  #define DEBUG_PRINTLN(x)      Serial.println(x)
  #define DEBUG_PRINTF(...)     Serial.printf(__VA_ARGS__)
  #define DEBUG_PRINTHEX(x)     Serial.printf("%02X ", (x))
  #define DEBUG_PRINTBIN32(x)   printBinary32(x)
#else
  #define DEBUG_BEGIN(x)        do {} while(0)
  #define DEBUG_PRINT(x)        do {} while(0)
  #define DEBUG_PRINTLN(x)      do {} while(0)
  #define DEBUG_PRINTF(...)     do {} while(0)
  #define DEBUG_PRINTHEX(x)     do {} while(0)
  #define DEBUG_PRINTBIN32(x)   do {} while(0)
#endif

#define SER   26
#define LAT   25
#define CLK   21
#define LED   0

#define B1    32
#define B2    33
#define DEBOUNCE 100
#define LONG_PRESS 1000

#define C0  0b00000011
#define C1  0b10011111
#define C2  0b00100101
#define C3  0b00001101
#define C4  0b10011001
#define C5  0b01001001
#define C6  0b01000001
#define C7  0b00011111
#define C8  0b00000001
#define C9  0b00011001
#define CB  0b11111111


#define MODE_YEARCLOCK  0
#define MODE_DATETIME   1
#define MODE_DEATHCLOCK 2
#define MODE_SETYEAR    3
#define MODE_SETMONTH   4
#define MODE_SETDAY     5
#define MODE_SETHOUR    6
#define MODE_SETMIN     7
#define MODE_SETDYEAR   8
#define MODE_SETDMONTH  9
#define MODE_SETDDAY    10

#define MAX_MODE        10

MCP7940_Class rtc;
uint32_t shift_reg = 0b00000000000000000000000000000000;
uint8_t segs[10] = {C0,C1,C2,C3,C4,C5,C6,C7,C8,C9};
uint8_t digits[12] = {CB,CB,CB,CB,CB,CB,CB,CB,CB,CB,CB,CB};
uint16_t dots = 0b0000000000001000;  // bits 0-11, 0 = decimal dot off, 1 = on

int i=0;
int d=4;

unsigned long last_B1_millis = 0;
unsigned long last_B2_millis = 0;

bool B1_state = false;
bool B2_state = false;
bool B1_longpressed = false;
bool B2_longpressed = false;

uint64_t year_decimal;
uint64_t death_clock_decimal;
int last_processed_second = -1;
double start_fraction;
double end_fraction;
double remaining_years_start;
double remaining_years_end;
unsigned long second_start_millis;

DateTime now;

uint8_t mode = MODE_YEARCLOCK;

int year;
int month;
int day;
int hour;
int minute;
int second;

int d_year = 2075;
int d_month = 1;
int d_day = 1;
int d_hour = 0;
int d_minute = 0;


/////////////////////////////////////////////////////////////////////////////////////////////
//  setup  //////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
void setup() {

//// RTC ////
  Wire.begin(15, 5);  // Special pin assignment for Qwiic connector on the Qwiic Pro Mini

  while (!rtc.begin()) {  // Initialize RTC communications
    Serial.println(F("Unable to find MCP7940M. Checking again in 3s."));  // Show error text
    delay(3000);                                                          // wait a second
  }  // of loop until device is located
  Serial.println(F("MCP7940 initialized."));
  while (!rtc.deviceStatus()) {  // Turn oscillator on if necessary
    Serial.println(F("Oscillator is off, turning it on."));
    bool deviceStatus = rtc.deviceStart();  // Start oscillator and return state
    if (!deviceStatus) {                        // If it didn't start
      Serial.println(F("Oscillator did not start, trying again."));  // Show error and
      delay(500);                                                   // wait for a second
    }                // of if-then oscillator didn't start
  }                  // of while the oscillator is off


//// PINS ////
  pinMode(SER, OUTPUT);
  pinMode(LAT, OUTPUT);
  pinMode(CLK, OUTPUT);
  pinMode(LED, OUTPUT);
  pinMode(B1, INPUT);
  pinMode(B2, INPUT);
  

//// DEBUG ////
  DEBUG_BEGIN(115200);
}



/////////////////////////////////////////////////////////////////////////////////////////////
//  loop  ///////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
void loop() {

  now = rtc.now();

  year = now.year();
  month = now.month();
  day = now.day();
  hour = now.hour();
  minute = now.minute();
  second = now.second();

  // Check if the second has changed to recalculate our interpolation start/end points
  if (second != last_processed_second) {
    last_processed_second = second;
    second_start_millis = millis();

    // --- Year Clock Calculation ---
    bool isLeap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    int daysInYear = isLeap ? 366 : 365;

    int dayOfYear = 0;
    for (int m = 1; m < month; m++) {
      if (m == 2) {
        dayOfYear += isLeap ? 29 : 28;
      } else if (m == 4 || m == 6 || m == 9 || m == 11) {
        dayOfYear += 30;
      } else {
        dayOfYear += 31;
      }
    }
    dayOfYear += day;
    
    double secondsInDay = (hour * 3600.0) + (minute * 60.0) + second;
    double fractionOfDay = secondsInDay / 86400.0;
    
    start_fraction = (dayOfYear - 1 + fractionOfDay) / daysInYear;
    
    double oneSecondInFractionOfYear = 1.0 / (daysInYear * 86400.0);
    end_fraction = start_fraction + oneSecondInFractionOfYear;

    // --- Death Clock Calculation ---
    DateTime deathDate(d_year, d_month, d_day, d_hour, d_minute, 0);
    unsigned long death_unixtime = deathDate.unixtime();
    unsigned long now_unixtime = now.unixtime();
    double seconds_in_average_year = 31556952.0; // 365.2425 days

    if (death_unixtime <= now_unixtime) {
        remaining_years_start = 0;
        remaining_years_end = 0;
    } else {
        unsigned long seconds_until_death_start = death_unixtime - now_unixtime;
        remaining_years_start = (double)seconds_until_death_start / seconds_in_average_year;

        // Calculate the remaining time for the *end* of the current second (which is 1 second less)
        unsigned long seconds_until_death_end = death_unixtime - (now_unixtime + 1);
        if (death_unixtime <= (now_unixtime + 1)) {
            remaining_years_end = 0;
        } else {
            remaining_years_end = (double)seconds_until_death_end / seconds_in_average_year;
        }
    }
  }

  // --- Interpolation ---
  unsigned long elapsed_millis = millis() - second_start_millis;
  if (elapsed_millis > 1000) {
    elapsed_millis = 1000; // Cap at 1000ms to prevent overshooting
  }
  double time_fraction = elapsed_millis / 1000.0;

  // Year Clock Interpolation
  double interpolated_fraction = start_fraction + ((end_fraction - start_fraction) * time_fraction);
  double yearClock = year + interpolated_fraction;
  long decimals = (long)((yearClock - year) * 100000000L);
  year_decimal = (uint64_t)year*100000000L + (uint64_t)decimals;

  // Death Clock Interpolation
  double interpolated_remaining_years = remaining_years_start + ((remaining_years_end - remaining_years_start) * time_fraction);
  if (interpolated_remaining_years < 0) interpolated_remaining_years = 0;

  uint64_t remaining_years_int_part = (uint64_t)interpolated_remaining_years;
  double remaining_fractional_part = interpolated_remaining_years - remaining_years_int_part;
  uint64_t remaining_decimal_scaled = (uint64_t)(remaining_fractional_part * 100000000.0);
  death_clock_decimal = remaining_years_int_part * 100000000ULL + remaining_decimal_scaled;


  #ifdef DEBUG
    if(mode == MODE_YEARCLOCK) {
      DEBUG_PRINT(year);
      DEBUG_PRINT(".");
      if (decimals < 10000000) DEBUG_PRINT("0");
      if (decimals < 1000000) DEBUG_PRINT("0");
      if (decimals < 100000) DEBUG_PRINT("0");
      if (decimals < 10000) DEBUG_PRINT("0");
      if (decimals < 1000) DEBUG_PRINT("0");
      if (decimals < 100) DEBUG_PRINT("0");
      if (decimals < 10) DEBUG_PRINT("0");
      DEBUG_PRINTLN(decimals);
    } else if (mode == MODE_DEATHCLOCK) {
      // similar debug for death clock if needed
    }
  #endif



  switch( mode ) {
    case MODE_YEARCLOCK:
      dots = 0b0000000000001000;
      show_num(year_decimal);
      break;
    case MODE_DATETIME:
      dots = 0b0000001010101000; 
      show_num((uint64_t)year*100000000L + (uint64_t)month*1000000L + (uint64_t)day*10000L + (uint64_t)hour*100L + (uint64_t)minute);
      break;
    case MODE_DEATHCLOCK:
      dots = 0b0000000000001000;
      show_num(death_clock_decimal);
      if(death_clock_decimal / 100000000L < 1000) digits[0] = CB;
      if(death_clock_decimal / 100000000L < 100) digits[1] = CB;
      if(death_clock_decimal / 100000000L < 10) digits[2] = CB;
      break;
    case MODE_SETYEAR:
      dots = 0b0000000000000000;
      show_num(year);
      for(i=0;i<8;i++) digits[i] = CB;
      break;
    case MODE_SETMONTH:
      dots = 0b0000000000000000;
      show_num(month);
      for(i=0;i<10;i++) digits[i] = CB;
      break;
    case MODE_SETDAY:
      dots = 0b0000000000000000;
      show_num(day);
      for(i=0;i<10;i++) digits[i] = CB;
      break;
    case MODE_SETHOUR:
      dots = 0b0000000000000000;
      show_num(hour);
      for(i=0;i<10;i++) digits[i] = CB;
      break;
    case MODE_SETMIN:
      dots = 0b0000000000000000;
      show_num(minute);
      for(i=0;i<10;i++) digits[i] = CB;
      break;
    case MODE_SETDYEAR:
      dots = 0b0000000000000000;
      show_num(d_year);
      for(i=0;i<8;i++) digits[i] = CB;
      digits[0] = 0b10000101;
      digits[1] = 0b01100011;
      break;
    case MODE_SETDMONTH:
      dots = 0b0000000000000000;
      show_num(d_month);
      for(i=0;i<10;i++) digits[i] = CB;
      digits[0] = 0b10000101;
      digits[1] = 0b01100011;
      break;
    case MODE_SETDDAY:
      dots = 0b0000000000000000;
      show_num(d_day);
      for(i=0;i<10;i++) digits[i] = CB;
      digits[0] = 0b10000101;
      digits[1] = 0b01100011;
      break;
  }



  handle_input();








  // UPDATE THE DISPLAY===========================================================

  // Turn on marked dots
  for(i=0;i<12;i++) if(dots & (1UL << i)) digits[i] &= ~0b00000001;

  // Clear the shift reg
  shift_reg = 0b00000000000000000000000000000000;

  // Set the next digit
  d++;
  if(d >= 8) {
    d=4;
    shift_reg &= ~(1U << 7);
  }
  shift_reg |= (1U << d);
  shift_reg &= ~(1U << d-1);

  // Set each display
  switch(d) {
    case 4:
      shift_reg |= (digits[11] << 8);    
      shift_reg |= (digits[7] << 16);    
      shift_reg |= (digits[3] << 24);    
      break;
    case 5:
      shift_reg |= (digits[10] << 8);    
      shift_reg |= (digits[6] << 16);    
      shift_reg |= (digits[2] << 24);    
      break;
    case 6:
      shift_reg |= (digits[9] << 8);    
      shift_reg |= (digits[5] << 16);    
      shift_reg |= (digits[1] << 24);    
      break;
    case 7:
      shift_reg |= (digits[8] << 8);    
      shift_reg |= (digits[4] << 16);    
      shift_reg |= (digits[0] << 24);    
      break;
  }

  // Set shift register
  set_shift(shift_reg);

  DEBUG_PRINTBIN32(shift_reg);
  DEBUG_PRINTLN("");
}

// hardware level very short pause
void pause(uint64_t span) {
  for (volatile int j = 0; j < span; j++) {
    __asm__ __volatile__ ("nop");
  }
}

// Push the 32 bits in 'bits' to the output of the shift registers
void set_shift(uint32_t bits) {
  for(int i=0; i<32; i++) {
    

    if(bits & (1UL <<i)) digitalWrite(SER, HIGH);//REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << SER));
    else digitalWrite(SER, LOW);//REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << SER));

    digitalWrite(CLK, LOW);//REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << CLK));

    pause(0);

    digitalWrite(CLK, HIGH);//REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << CLK));
    pause(0);
  }

  digitalWrite(LAT, HIGH);//REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << LAT));
  pause(0);
  digitalWrite(LAT, LOW);//REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << LAT));
}


void printBinary32(uint32_t value) {
  for(int8_t i=31; i >= 0; i--) {
    DEBUG_PRINT((value >> i) & 1);

  }
}

void show_num(uint64_t number) {
  
  for(i=11;i>=0;i--) {
    digits[i] = segs[number % 10];
    number /= 10;
  }

}

void handle_input(void) {

  if(!digitalRead(B1) && B1_state == false) {  // new B1 button press started
    last_B1_millis = millis();
    B1_state = true;
  }

  if(!digitalRead(B1) && B1_state == true) {
    if(millis()-last_B1_millis > DEBOUNCE) {
      if(millis() - last_B1_millis > LONG_PRESS) {
        mode++;
        if(mode > MAX_MODE) mode = 0;
        last_B1_millis = millis();
        B1_longpressed = true;
      }
    }
  }

  if(digitalRead(B1)) {
    B1_state = false;
    if(millis()-last_B1_millis > DEBOUNCE && B1_longpressed == false) {
      switch(mode) {
        case MODE_SETYEAR:
          rtc.adjust(DateTime(year+1, month, day, hour, minute, second));
          break;
        case MODE_SETMONTH:
          rtc.adjust(DateTime(year, month+1, day, hour, minute, second));
          break;
        case MODE_SETDAY:
          rtc.adjust(DateTime(year, month, day+1, hour, minute, second));
          break;
        case MODE_SETHOUR:
          rtc.adjust(DateTime(year, month, day, hour+1, minute, second));
          break;
        case MODE_SETMIN:
          rtc.adjust(DateTime(year, month, day, hour, minute+1, 0));
          break;
        case MODE_SETDYEAR:
          d_year++;
          break;
        case MODE_SETDMONTH:
          d_month++;
          if(d_month > 12) d_month = 1;
          break;
        case MODE_SETDDAY:
          d_day++;
          if(d_day > 31) d_day = 1;
          break;
      }

    }

    last_B1_millis = millis();
    B1_longpressed = false;
  }


 
  if(!digitalRead(B2) && B2_state == false) {  // new B2 button press started
    last_B2_millis = millis();
    B2_state = true;
  }

  if(!digitalRead(B2) && B2_state == true) {
    if(millis()-last_B2_millis > DEBOUNCE) {
      if(millis() - last_B2_millis > LONG_PRESS) {
        mode--;
        if(mode < 0) mode = MAX_MODE;
        last_B2_millis = millis();
        B2_longpressed = true;
      }
    }
  }

  if(digitalRead(B2)) {
    B2_state = false;
    if(millis()-last_B2_millis > DEBOUNCE && B2_longpressed == false) {
      switch(mode) {
        case MODE_SETYEAR:
          rtc.adjust(DateTime(year-1, month, day, hour, minute, second));
          break;
        case MODE_SETMONTH:
          rtc.adjust(DateTime(year, month-1, day, hour, minute, second));
          break;
        case MODE_SETDAY:
          rtc.adjust(DateTime(year, month, day-1, hour, minute, second));
          break;
        case MODE_SETHOUR:
          rtc.adjust(DateTime(year, month, day, hour-1, minute, second));
          break;
        case MODE_SETMIN:
          rtc.adjust(DateTime(year, month, day, hour, minute-1, 0));
          break;
        case MODE_SETDYEAR:
          d_year--;
          break;
        case MODE_SETDMONTH:
          d_month--;
          if(d_month < 1) d_month = 12;
          break;
        case MODE_SETDDAY:
          d_day--;
          if(d_day < 1) d_day = 31;
          break;
      }

    }

    last_B2_millis = millis();
    B2_longpressed = false;
  }


    
}