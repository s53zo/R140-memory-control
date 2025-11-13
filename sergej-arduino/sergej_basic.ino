// Vasilij & Sergej remote control - S53M Team _ old version

const byte Mask = 15;
uint8_t band;
uint8_t bandOld;
const int pttInp = 7;
const int pttOut = 6;
const int uglasevanje = A5;
void setup() {
  Serial.begin(9600);
  pinMode(pttOut,OUTPUT);
  pinMode(pttInp,INPUT_PULLUP);
  pinMode(uglasevanje,INPUT_PULLUP);
  digitalWrite(pttOut,LOW);
  DDRD = DDRD | B00111100; //2,3,4,5 je OUTPUT
  DDRC = DDRC & Mask;
  PORTC = PORTC | Mask;
}
void loop() {
  int in_ptt = digitalRead(pttInp);
  int in_ugl = digitalRead(uglasevanje);
  if(in_ptt == LOW){
    digitalWrite(pttOut,HIGH);
  }else{
    digitalWrite(pttOut,LOW);
    bandControl();
  }
}
void bandControl() {
  band = PINC & Mask; 
  if(band == bandOld){  // ne delamo nič
    Serial.print("Obstojece: ");
    Serial.println(band);
  }else{  //zamenjamo band
    Serial.print("NOVO: ");
    Serial.println(band);
    bandOld = band;
    switch (band) {
    case 15:
      PORTD |= B00111100; 
      break;
    case 14:
      PORTD &= B11000011;
      break;
    case 13:
      PORTD &= B11000011;
      PORTD |= B00000100;
      break;
    case 12:
      PORTD &= B11000011;
      PORTD |= B00001000;
      break;
    case 11:
      PORTD &= B11000011;
      PORTD |= B00001100;
      break;
    case 10:
      PORTD &= B11000011;
      PORTD |= B00010000;
      break;
    case 9:
      PORTD &= B11000011;
      PORTD |= B00010100;
      break;
    case 8:
      PORTD &= B11000011;
      PORTD |= B00011000;
      break;
    case 7:
      PORTD &= B11000011;
      PORTD |= B00011100;
      break;
    case 6:
      PORTD &= B11000011;
      PORTD |= B00100000;
      break;
    case 5:
      PORTD &= B11000011;
      PORTD |= B00100100;
      break;
    default:
      PORTD |= B00111100;
      break;
    }
  }
  delay(100);
}
