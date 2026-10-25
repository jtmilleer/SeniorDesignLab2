// lab2_filter_sketch_fast: same filters, threshold logic and alert pin as lab2_filter_sketch,
// with a faster loop so mode 1 keeps up at Fs = 3000 Hz (333 us per sample).
// The original printed "MISSED A SAMPLE" ~2666 times a second in mode 1: each sample took
// ~375 us, so the Uno really sampled at ~2670 Hz and mode 1's band slid down to ~378-396 Hz,
// missing the 440.9 Hz transmitter.
// Changes: skip the zero numerator taps, circular s[] buffer, max-of-10 once per second,
// misses counted and printed once per second, "LOOP us" timing line, unsigned long changet,
// and samples scheduled at exactly 3000 Hz (micros() steps by 4 us, so the template ran at ~2950 Hz).
// Check: "LOOP us" must stay under 333 and no "MISSED" lines should appear.
// Same ALERT_PIN output to the ESP32 as lab2_filter_sketch.

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
int m = 10; // number of past outputs to average for hysteresis

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


float x[n],y[n],yn, s[10];     // Space to hold previous samples and outputs; n'th order filter will require upto n samples buffered
int sk = 0;                    // next slot in s[] (circular, so s[] is not shifted every sample)

float threshold_val = 0.15; // Threshold value. Anything higher than the threshold will turn the LED off, anything lower will turn the LED on

// Beam state, reported to the ESP32 as a digital level on ALERT_PIN (HIGH = breached);
// the ESP32 turns each change into an alert.
bool breached = false;

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

   for(i = 0; i<m; i++)
    s[i] = 0;
   yn = 0;
}



void loop()
{
   unsigned long t1;
   int i,count,val;
   int k=0;
   unsigned long changet = micros();   // unsigned long: a float stops the 1 s check after ~71 min
   int missed = 0;                     // samples that overran Ts this second
   unsigned long maxBusy = 0;          // longest sample this second, in us

   byte k3 = 0;                        // adds 1 us every 3rd sample: average period 333.33 us = exactly 3000 Hz

   count = 0;
   t1 = micros();
   while (1) {
      unsigned long start = micros();   // when this sample's work actually began

      // Calculate the next value of the difference equation.

      for(i=n-1; i>0; i--){             // Shift samples
         x[i] = x[i-1];                                            
         y[i] = y[i-1];
      }
      
      val = analogRead(analogPin);  // New input

      x[0] = val*(5.0/1023.0)-2.5;  // Scale to match ADC resolution and range

      yn = num[0] * x[0];
      
      for(i=1;i<n;i++)             // Incorporate previous outputs (y[n])
         yn = yn - den[i]* y[i];
      for(i=2;i<n;i+=2)            // Odd numerator taps are 0 in every mode, so skip them (saves time)
         yn = yn + num[i] * x[i];
         

       y[0] = yn;                  // New output

      //  The variable yn is the output of the filter at this time step.
      //  Now we can use it for its intended purpose:
      //       - Apply theshold
      //       - Apply hysteresis
      //       - What to do when the beam is interrupted, turn on a buzzer, send SMS alert.
      //       - etc.

      s[sk] = abs(2*yn);  // Absolute value of the filter output.
      if (++sk == m) sk = 0;

      unsigned long busy = micros() - start;   // work time for this sample (must stay under Ts)
      if (busy > maxBusy) maxBusy = busy;
      if (busy > Ts) missed++;                  // too slow for 3000 Hz

      // Check the output value against the threshold value every 10^6 microseconds or 1 second
      if ((micros()-changet) > 1000000UL)
      {
        // SAMPLE Hystersis: Take the max of the past 10 samples and compare that with the threshold
        // (computed here, once per second, instead of on every sample, to save time)
        float maxs = 0;
        for(int i = 0; i< m; i++)
        {
          if (s[i]>maxs)
            maxs = s[i];
        }

        Serial.println(maxs, 3);
        Serial.print("LOOP us ");      // longest sample this second (not counting this print); must stay under 333
        Serial.println(maxBusy);
        if (missed > 0)                // any count here means the filter is too slow for 3000 Hz
        {
          Serial.print("MISSED ");
          Serial.println(missed);
        }
        missed = 0;
        maxBusy = 0;

        changet = micros();
        bool nowBreached = maxs < threshold_val;
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

     
      // Misses are counted above (busy > Ts) and printed once per second: printing every miss
      // at 1200 baud fills the TX buffer, Serial blocks, and every following sample is missed too.
      // The once-per-second print itself runs long; the schedule below catches up right after.
      while((micros()-t1) < Ts);
      // Schedule the next sample from the previous start time instead of re-reading micros():
      // micros() counts in 4 us steps, so "t1 = micros()" made each period 336-340 us
      // (~2950 Hz), which moved mode 1's band away from the 440.9 Hz transmitter.
      t1 += Ts;
      if (++k3 == 3) { k3 = 0; t1 += 1; }
      
   }

}
