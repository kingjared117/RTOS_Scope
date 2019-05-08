//Sam Shurberg & Jared Goldman
//April 13, 2017
//Lab2

#include <xdc/std.h>

#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>
#include <xdc/cfg/global.h>

#include <ti/sysbios/BIOS.h>

#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Event.h>


#include "inc/hw_types.h"
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/lm3s8962.h"
#include "driverlib/sysctl.h"
#include "drivers/rit128x96x4.h"
#include "frame_graphics.h"
#include "utils/ustdlib.h"
#include "driverlib/timer.h"
#include "driverlib/interrupt.h"
#include "driverlib/gpio.h"
#include "driverlib/adc.h"
#include "stdio.h"
#include "buttons.h"
#include "math.h"
#include "kiss_fft.h"
#include "_kiss_fft_guts.h"


#define BUTTON_CLOCK 200 // button scanning interrupt rate in Hz
#define ADC_BUFFER_SIZE 2048 // must be a power of 2
#define ADC_BUFFER_WRAP(i) ((i) & (ADC_BUFFER_SIZE - 1)) // index wrapping macro
#define FIFO_SIZE 11
#define SCREEN_SIZE_X 127
#define SCREEN_SIZE_Y 96
#define	ADC_OFFSET 563
#define VIN_RANGE 6
#define PIXELS_PER_DIV 12
#define ADC_BITS 10
#define NUM_MSG
#define PI 3.1415926535897
#define FFT_LEN 1024 //FFT length
#define KISS_FFT_CFG_SIZE (sizeof(struct kiss_fft_state) + sizeof(kiss_fft_cpx)*(FFT_LEN-1))
#define SPECTRUM_SIZE 1024 //Size of spectrum array

unsigned long g_ulSystemClock;

volatile int FFT = 0;
volatile int g_iADCBufferIndex = ADC_BUFFER_SIZE - 1; // latest sample index
volatile short g_psADCBUFFER[ADC_BUFFER_SIZE]; // circular buffer
volatile unsigned long g_ulADCErrors = 0; // number of missed ADC deadlines
volatile int trigger_state = 0; //default falling edge trigger
volatile int trigger;
volatile float fVoltsPerDiv;
volatile int scale = 1;
volatile float fscale;
volatile short Time_Select = 1;
volatile short Trig_Select = 0;
volatile short Volt_Select = 0;
volatile int copy[128];
volatile float out1[128];

size_t buffer_size = KISS_FFT_CFG_SIZE;
static char kiss_fft_cfg_buffer[KISS_FFT_CFG_SIZE]; //Configure Kiss FFT memory


static kiss_fft_cpx in[FFT_LEN]; //complex waveform buffer
static kiss_fft_cpx out[FFT_LEN];//spectrum buffer
volatile unsigned short spect[SPECTRUM_SIZE]; //Array for spectrum drawing


//Used to display scaling in a "clean" way
const char * const g_ppcVoltageScaleStr[] = {"100 mV", "200 mV", "500 mV", "1 V"};

void UserInputTask(UArg arg0, UArg arg1);
void displayTask(UArg arg0, UArg arg1);
void waveformTask(UArg arg0, UArg arg1);
void FFT_Task(UArg arg0, UArg arg1);
//void TimerISR(UArg arg0, UArg arg1);


void ADC_ISR(void)
{
	ADC0_ISC_R = 1; // clear ADC sequence0 interrupt flag in the ADCISC register

	if (ADC0_OSTAT_R & ADC_OSTAT_OV0) { // check for ADC FIFO overflow
		g_ulADCErrors++; // count errors - step 1 of the signoff
		ADC0_OSTAT_R = ADC_OSTAT_OV0; // clear overflow condition
	}

	int buffer_index = ADC_BUFFER_WRAP(g_iADCBufferIndex + 1);
	g_psADCBUFFER[buffer_index] = ADC_SSFIFO0_R; // read sample from the ADC sequence0 FIFO
	g_iADCBufferIndex = buffer_index;
}


void TimerISR(void) {
	unsigned long presses = g_ulButtons;
	char activeButton;

	TIMER0_ICR_R = 1; // clear interrupt flag
	ButtonDebounce(((~GPIO_PORTF_DATA_R & GPIO_PIN_1) >> 1) | ((~GPIO_PORTE_DATA_R & (GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3)) << 1));
	presses = ~presses & g_ulButtons; // button press detector

	//Read button presses and assign correct value for scaling and mode changes later


	//Switching between FFT and Trigger options.
	if((presses & 1) && Trig_Select){
		activeButton = 'T';
		Mailbox_post(mailbox0, &activeButton, BIOS_NO_WAIT);
	}else if ((presses & 1) && (Volt_Select || Time_Select)){
		activeButton = 'F';
		Mailbox_post(mailbox0, &activeButton, BIOS_NO_WAIT);
	}else if(presses & 2){
		activeButton = 'U';
		Mailbox_post(mailbox0, &activeButton, BIOS_NO_WAIT);
	}else if(presses & 4){
		activeButton = 'D';
		Mailbox_post(mailbox0, &activeButton, BIOS_NO_WAIT);
	}else if(presses & 8){
		activeButton = 'L';
		Mailbox_post(mailbox0, &activeButton, BIOS_NO_WAIT);
	}else if(presses & 16){
		activeButton = 'R';
		Mailbox_post(mailbox0, &activeButton, BIOS_NO_WAIT);
	}

	Semaphore_post(sem_user);
}





/*unsigned long cpu_load_count(void)
{
	unsigned long i = 0;
	TimerIntClear(TIMER3_BASE, TIMER_TIMA_TIMEOUT);
	TimerEnable(TIMER3_BASE, TIMER_A); // start one-shot timer
	while (!(TimerIntStatus(TIMER3_BASE, 0) & TIMER_TIMA_TIMEOUT))
		i++;
	return i;
}*/

int main(void) {

//	unsigned long ulDivider, ulPrescaler;

	IntMasterDisable();

//	unsigned long count_unloaded = 0;
//	unsigned long count_loaded = 0;
//	float cpu_load = 0.0;

	// initialize the clock generator
//	if (REVISION_IS_A2)
//	{
//		SysCtlLDOSet(SYSCTL_LDO_2_75V);
//	}
//	SysCtlClockSet(SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL | SYSCTL_OSC_MAIN |
//			SYSCTL_XTAL_8MHZ);
//	g_ulSystemClock = SysCtlClockGet();




	RIT128x96x4Init(3500000); // initialize the OLED display

//	char pcStr[50]; // string buffer
//	unsigned long ulTime; // local copy of g_ulTime

	// initialize a general purpose timer for periodic interrupts


	// configure GPIO used to read the state of the on-board push buttons
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
	GPIOPinTypeGPIOInput(GPIO_PORTF_BASE, GPIO_PIN_1);
	GPIOPadConfigSet(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
	GPIOPinTypeGPIOInput(GPIO_PORTE_BASE, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3);
	GPIOPadConfigSet(GPIO_PORTE_BASE, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3, GPIO_STRENGTH_2MA,
		 GPIO_PIN_TYPE_STD_WPU);

	SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0); // enable the ADC
	SysCtlADCSpeedSet(SYSCTL_ADCSPEED_500KSPS); // specify 500ksps
	ADCSequenceDisable(ADC0_BASE, 0); // choose ADC sequence 0; disable before configuring
	ADCSequenceConfigure(ADC0_BASE, 0, ADC_TRIGGER_ALWAYS, 0); // specify the "Always" trigger
	ADCSequenceStepConfigure(ADC0_BASE, 0, 0, ADC_CTL_CH0 | ADC_CTL_IE | ADC_CTL_END); // in the 0th step, sample channel 0
	// enable interrupt, and make it the end of sequence
	ADCIntEnable(ADC0_BASE, 0); // enable ADC interrupt from sequence 0
	ADCSequenceEnable(ADC0_BASE, 0); // enable the sequence. it is now sampling




//	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER3);
//	TimerDisable(TIMER3_BASE, TIMER_BOTH);
//	TimerConfigure(TIMER3_BASE, TIMER_CFG_ONE_SHOT);
//	TimerLoadSet(TIMER3_BASE, TIMER_A, g_ulSystemClock - .02); // 20 msec interval

//	count_unloaded = cpu_load_count();

	BIOS_start();
}

//Read Button Presses and adjust mode and scaling. Comes almost straight from Lab1
void UserInputTask(UArg arg0, UArg arg1)
{
	char msg;

	while(1){
		Semaphore_pend(sem_user, BIOS_WAIT_FOREVER);
		if(Mailbox_pend(mailbox0, &msg, BIOS_WAIT_FOREVER)){

			switch(msg){
			case 'T':
				trigger_state = !trigger_state;
				break;
			case 'F':
				FFT = !FFT;
				break;
			case 'U':
				if(scale < 4 && Volt_Select){
					scale = scale + 1;
					break;
				}
				else
					break;

			case 'D':
				if(scale > 1 && Volt_Select){
					scale = scale-1;
					break;
				}
				else
					break;
			case 'L':
				if(Trig_Select){
					Trig_Select = 0;
					Volt_Select = 1;
				}
				else if(Volt_Select){
					Volt_Select = 0;
					Time_Select = 1;
				}
				break;
			case 'R':
				if(Volt_Select){
					Volt_Select = 0;
					Trig_Select = 1;
				}
				else if(Time_Select){
					Time_Select = 0;
					Volt_Select = 1;
				}
				break;
			}

		//Proper coefficients assigned for scale options
		if(scale == 1)
			fVoltsPerDiv = 0.1;
		else if(scale == 2)
			fVoltsPerDiv = 0.2;
		else if(scale == 3)
			fVoltsPerDiv = 0.5;
		else if(scale == 4)
			fVoltsPerDiv = 1;

		Semaphore_post(sem_waveform);
		}
	}
}

void displayTask(UArg arg0, UArg arg1) {
	int j;
	int l;

	IntMasterEnable();

	while(1){
		FillFrame(0);

	if(Semaphore_pend(sem_display_task, BIOS_WAIT_FOREVER)){

		//Draw Background
		//Horizontal Lines
		DrawLine(0, 47, 127, 47, 6);
		DrawLine(0, 11, 127, 11, 4);
		DrawLine(0, 23, 127, 23, 4);
		DrawLine(0, 35, 127, 35, 4);
		DrawLine(0, 59, 127, 59, 4);
		DrawLine(0, 71, 127, 71, 4);
		DrawLine(0, 83, 127, 83, 4);

		//Vertical Lines

		DrawLine(124, 0, 124, 95, 2);
		DrawLine(112, 0, 112, 95, 2);
		DrawLine(100, 0, 100, 95, 2);
		DrawLine(88, 0, 88, 95, 2);
		DrawLine(76, 0, 76, 95, 2);
		DrawLine(64, 0, 64, 95, 4);
		DrawLine(52, 0, 52, 95, 2);
		DrawLine(40, 0, 40, 95, 2);
		DrawLine(28, 0, 28, 95, 2);
		DrawLine(16, 0, 16, 95, 2);
		DrawLine(4, 0, 4, 95, 2);

		if(!FFT){
			//Draw Trigger Symbols
			//Rising Edge Trigger
			if(trigger_state){
				DrawLine(98, 3, 105, 3, 10);
				DrawLine(104, 3, 105, 8, 10);
				DrawLine(104, 8, 111, 8, 10);
				DrawLine(101, 4, 105, 7, 10);
				DrawLine(104, 7, 108, 4, 10);
			}

			//Falling Edge Trigger
			else{
				DrawLine(98, 8, 105, 8, 10);
				DrawLine(104, 8, 105, 3, 10);
				DrawLine(104, 3, 111, 3, 10);
				DrawLine(102, 7, 105, 4, 10);
				DrawLine(104, 4, 108, 7, 10);
			}

			//Draw Scale Values
			if(scale == 1)
				DrawString(48, 0, g_ppcVoltageScaleStr[0], 15, false);
			else if(scale == 2)
				DrawString(48, 0, g_ppcVoltageScaleStr[1], 15, false);
			else if(scale == 3)
				DrawString(48, 0, g_ppcVoltageScaleStr[2], 15, false);
			else if(scale == 4)
				DrawString(48, 0, g_ppcVoltageScaleStr[3], 15, false);

			//Draw Time Scaling
			char Time_Scale[5] = "24us";
			DrawString(0, 0, Time_Scale, 15, false);

			float fScale;

		//Determine the correct scaled waveform to be drawn

			fScale = (VIN_RANGE * PIXELS_PER_DIV)/((1 << ADC_BITS) * fVoltsPerDiv);

		for(j = 0; j < 128; j++){
			copy[j] = FRAME_SIZE_Y/2 - (int)roundf((copy[j] - ADC_OFFSET) * fScale);
		}


			//Draw Signal on Screen
			for(l = 0; l < 127; l++){
				DrawLine(l, copy[l], l + 1, copy[l+1], 8);
			}
	}


	//Draw the FFT
	else{
		const char * FFT_TITLE = "FFT MODE";
		DrawString(0, 0, FFT_TITLE, 15, 0);

		const char * FFT_SCALE = "20dBV";
		DrawString(80, 0, FFT_SCALE,15 ,0);

		int x,z, y = 0;
		for(x = 0; x < 128; x++){
			//Calculate the FFT
			z = (int)round(20 * (log10((out[x].r) * (out[x].r))));
			if(x > 0)
				DrawLine(x, z, x-1, y, 8);
			y = z;
		}
		Semaphore_post(sem_fft);
	}


	RIT128x96x4ImageDraw(g_pucFrame, 0, 0, FRAME_SIZE_X, FRAME_SIZE_Y);

	Semaphore_post(sem_waveform);
}
}
}

void waveformTask(UArg arg0, UArg arg1){
	int buffer_index;
	int i;
	int k;

	while(1){
		if(Semaphore_pend(sem_waveform, BIOS_WAIT_FOREVER)){
		if(!FFT){
		buffer_index = ADC_BUFFER_WRAP(g_iADCBufferIndex - 64);

		//check for half of the buffer size
		for(i = 0; i < 1024; i++){
			buffer_index = ADC_BUFFER_WRAP(buffer_index - i);

				//Check on the rising edge of trigger_value
				if(trigger_state){

					if(g_psADCBUFFER[ADC_BUFFER_WRAP(buffer_index-1)] <= 563 && (g_psADCBUFFER[ADC_BUFFER_WRAP(buffer_index)]) >= 563) {
						trigger = buffer_index;
						break;
					}
				}
				//Check on Falling Edge
				else {
					if(g_psADCBUFFER[ADC_BUFFER_WRAP(buffer_index-1)] >= 563 && (g_psADCBUFFER[ADC_BUFFER_WRAP(buffer_index)]) <= 563){
						trigger = buffer_index;
						break;
					}

				}
		}


		//Copy to other half of waveform
		int temp_trigger = trigger - 64;
		for(k = 0; k <= 127; k++) {
			copy[k] = g_psADCBUFFER[ADC_BUFFER_WRAP(temp_trigger)];
			temp_trigger++;
		}
		Semaphore_post(sem_display_task);
	}
	
	
	// FFT MODE
	else{
		int h;

		int fftBuffer = g_iADCBufferIndex;

		for(h = 0; h < 1024; h++)
		{
			spect[h] = g_psADCBUFFER[ADC_BUFFER_WRAP(fftBuffer)];
			fftBuffer= fftBuffer - 1;
		}
		Semaphore_post(sem_fft);
	}
	}
}
}

void FFT_Task(UArg arg0, UArg arg1){
	kiss_fft_cfg cfg; //Configure Kiss FFT
	//int i;
	int j;

	while(1){
		if(Semaphore_pend(sem_fft, BIOS_WAIT_FOREVER)){
			cfg = kiss_fft_alloc(FFT_LEN, 0, kiss_fft_cfg_buffer, &buffer_size); 			//initalize Kiss FFT

			for(j = 0; j < FFT_LEN; j++){ //Generate input wave
				in[j].r = spect[j];	//real part of wave
				in[j].i = 0;	//imaginary part of wave
			}
			kiss_fft(cfg, in, out); //compute fft

			Semaphore_post(sem_display_task);
		}
	}
}



//	while (true) {
//		FillFrame(0); // clear frame buffer
//		ulTime = g_ulTime; // read shared global only once
//		usprintf(pcStr, "Time = %02u:%02u:%02u", ((((ulTime-ulTime%100)/100)/60)%60),(((ulTime-ulTime%100)/100)%60),(ulTime%100)); // convert time to string
//		DrawString(0, 0, pcStr, 15, false); // draw string to frame buffer
		// copy frame to the OLED screen
		//RIT128x96x4ImageDraw(g_pucFrame, 0, 0, FRAME_SIZE_X, FRAME_SIZE_Y);














		//count_loaded = cpu_load_count();
		//cpu_load = 1.0 - (float)count_loaded/count_unloaded; // compute CPU load

		//char CPUStr[17];

		//sprintf(CPUStr, "CPU Load = %f", cpu_load);


		//DrawString(0, 84, CPUStr, 15, false);




	//}




//	return 0;
//}
