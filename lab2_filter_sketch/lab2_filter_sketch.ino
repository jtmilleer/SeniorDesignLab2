// A digital frequency selective filter
// A. Kruger, 2019
// revised R. Mudumbai, 2020 & 2024
// revised N. Najeeb, 2025



// The following defines are used for setting and clearing register bits
// on the Arduino processor. Low-level stuff: leave alone.

#ifndef cbi
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#endif

#ifndef sbi
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#endif

int analogPin = A0;     // Specify analog input pin. Make sure to keep between 0 and 5V.
int LED = 12;           // Specify output analog pin with indicator LED 
int ALERT_PIN = 8;      // HIGH while the beam is broken; wired to the ESP32 (see esp32_alert)

   // num and den are the numerator and denominator coeffs of a digital frequency-selective filter
   // designed for a sample rate Fs=3000 HZ
  

const int n = 7;   // number of past input and output samples to buffer; change this to match order of your filter

// Select which filter to run:
//   0 = 120 Hz lamp detector (TA check-in)
//   1 = 425-445 Hz bandpass
//   2 = 490-510 Hz bandpass
//   3 = 525-545 Hz bandpass
#define FILTER_MODE 1

#if FILTER_MODE == 0
// 2nd order resonator near 120 Hz
float den[] = {1.0000,   -1.866752, 	0.935902,	0.00,	0.00,	0.00,	0.00}; //Denominator Coefficients
float num[] = {0.032049,   0.00,   -0.032049,         0.00,    0.00,    0.00,   0.00}; //Numerator Coefficients
#elif FILTER_MODE == 1
float den[] = {1, -3.6268947, 7.3014445, -8.8201584, 7.1003574, -3.4298617, 0.91963178}; //Denominator Coefficients
float num[] = {8.8140061e-06, 0, -2.6442018e-05, 0, 2.6442018e-05, 0, -8.8140061e-06}; //Numerator Coefficients
#elif FILTER_MODE == 2
float den[] = {1, -2.9587641, 5.8346086, -6.7130303, 5.673925, -2.7980276, 0.91963178}; //Denominator Coefficients
float num[] = {8.8140061e-06, 0, -2.6442018e-05, 0, 2.6442018e-05, 0, -8.8140061e-06}; //Numerator Coefficients
#elif FILTER_MODE == 3
float den[] = {1, -2.5754923, 5.1274985, -5.6410495, 4.986292, -2.4355773, 0.91963178}; //Denominator Coefficients
float num[] = {8.8140061e-06, 0, -2.6442018e-05, 0, 2.6442018e-05, 0, -8.8140061e-06}; //Numerator Coefficients
#else
#error "FILTER_MODE must be 0, 1, 2, or 3"
#endif


float x[n],y[n],yn;     // Space to hold previous samples and outputs; n'th order filter will require upto n samples buffered

float env = 0;                    // smoothed envelope of the filter output
const float ENV_ALPHA = 0.01;     // envelope smoothing; ~100 samples (33 ms) time constant

float threshold_val = 0.16; // Threshold value. Anything higher than the threshold will turn the LED off, anything lower will turn the LED on
const float HYSTERESIS = 0.2;  // breach below threshold_val*0.8, clear above threshold_val*1.2

// Beam state, reported to the ESP32 as a digital level on ALERT_PIN (HIGH = breached);
// the ESP32 turns each change into an alert.
bool breached = false;

unsigned int missed = 0;   // samples that overran Ts since the last status print

// time between samples Ts = 1/Fs. If Fs = 3000 Hz, Ts=333 us
int Ts = 333;

void setup()
{
   Serial.begin(1200);
   int i;

   sbi(ADCSRA,ADPS2);     // Next three lines make the ADC run faster
   cbi(ADCSRA,ADPS1);
   cbi(ADCSRA,ADPS0);

   pinMode(LED,OUTPUT);   // Makes the LED pin an output
   pinMode(ALERT_PIN,OUTPUT);

   for (i=0; i<n; i++)
      x[i] = y[i] = 0;
   yn = 0;
}



void loop()
{
   unsigned long t1;
   int i,count,val;
   int k=0;
   float changet = micros();

   count = 0;
   while (1) {
      t1 = micros();

      // Calculate the next value of the difference equation.

      for(i=n-1; i>0; i--){             // Shift samples
         x[i] = x[i-1];                                            
         y[i] = y[i-1];
      }
      
      val = analogRead(analogPin);  // New input

      x[0] = val*(5.0/1023.0)-2.5;  // Scale to match ADC resolution and range

      yn = num[0] * x[0];
      
      for(i=1;i<n;i++)             // Incorporate previous outputs (y[n])
         yn = yn - den[i]* y[i] + num[i] * x[i];          
         

       y[0] = yn;                  // New output

      //  The variable yn is the output of the filter at this time step.
      //  Now we can use it for its intended purpose:
      //       - Apply theshold
      //       - Apply hysteresis
      //       - What to do when the beam is interrupted, turn on a buzzer, send SMS alert.
      //       - etc.

      // Envelope detector: a running average of |2*yn| (time constant ~1/ENV_ALPHA samples)
      // averages out noise spikes instead of latching onto them like a max over a few samples.
      // The average of |sin| is 2/pi of its peak, so scale by pi/2 to keep the old peak scale
      // and the same threshold_val.
      env += ENV_ALPHA * (abs(2*yn) - env);
      float maxs = env * 1.5708;


      
      // Check the output value against the threshold value every 10^6 microseconds or 1 second
      if ((micros()-changet) > 1000e3)
      {
        Serial.println(maxs);
        if (missed > 0)
        {
          Serial.print("MISSED ");
          Serial.println(missed);
          missed = 0;
        }

        changet = micros();
        // Hysteresis: once breached, the signal must climb back above the upper threshold to clear,
        // so a level hovering near threshold_val doesn't toggle the state back and forth.
        bool nowBreached = breached ? maxs < threshold_val * (1 + HYSTERESIS)
                                    : maxs < threshold_val * (1 - HYSTERESIS);
        digitalWrite(LED, nowBreached ? HIGH : LOW);
        digitalWrite(ALERT_PIN, nowBreached ? HIGH : LOW);

        if (nowBreached != breached)
        {
          breached = nowBreached;
          Serial.println(breached ? "BREACH" : "CLEAR");   // USB serial monitor only
        }
      }
      
      // The filter was designed for a 3000 Hz sampling rate. This corresponds
      // to a sample every 333 us. The code above must execute in less time
      // (if it doesn't, it is not possible to do this filtering on this processor).
      // Below we tread some water until it is time to process the next sample

     
      if((micros()-t1) > Ts)
      {
		// if this happens, you must reduce Fs, and/or simplify your filter to run faster.
		// Count it instead of printing here: at 1200 baud a print per miss fills the TX buffer,
		// Serial blocks, and every following sample is missed too.
		missed++;
      }
      while((micros()-t1) < Ts);  
      
   }

}
