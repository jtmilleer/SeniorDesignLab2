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

   // num and den are the numerator and denominator coeffs of a digital frequency-selective filter
   // designed for a sample rate Fs=3000 HZ
  

const int n = 7;   // number of past input and output samples to buffer; change this to match order of your filter
int m = 10; // number of past outputs to average for hysteresis

// Select which filter to run:
//   0 = 120 Hz lamp detector (TA check-in)
//   1 = 425-445 Hz bandpass
//   2 = 490-510 Hz bandpass
//   3 = 525-545 Hz bandpass
#define FILTER_MODE 0

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

float threshold_val = 0.2; // Threshold value. Anything higher than the threshold will turn the LED off, anything lower will turn the LED on

// Beam state reported to the ESP32 over serial (TX pin 1). A line is only sent when the
// state changes, as "EVT BREACH <maxs>" or "EVT CLEAR <maxs>"; the ESP32 turns it into an alert.
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
   float changet = micros();

   count = 0;
   while (1) {
      t1 = micros();

      // Calculate the next value of the difference equation.

      for(i=n-1; i>0; i--){             // Shift samples
         x[i] = x[i-1];                                            
         y[i] = y[i-1];
      }
      
      for(i=m-1; i>0; i--){             // Shift absoulute output
         s[i] = s[i-1];
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

      s[0] = abs(2*yn);  // Absolute value of the filter output.

      // SAMPLE Hystersis: Take the max of the past 10 samples and compare that with the threshold
      float maxs = 0;
      for(int i = 0; i< m; i++)
      {
        if (s[i]>maxs)
          maxs = s[i];
      }


      
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
        bool nowBreached = maxs < threshold_val;
        digitalWrite(LED, nowBreached ? HIGH : LOW);

        if (nowBreached != breached)
        {
          breached = nowBreached;
          Serial.print(breached ? "EVT BREACH " : "EVT CLEAR ");
          Serial.println(maxs, 3);
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
