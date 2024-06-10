const mqtt = require('mqtt');
const {MongoClient} = require('mongodb');
const mongoose = require('mongoose');
const moment = require('moment');
const shortId = require('shortid');
const Events = require('./event');

const topic_fan_control = "smarthome/fan/control";
const topic_fan_timer = "smarthome/fan/time";
const topic_sensor = "smarthome/sensor";

const client = mqtt.connect('mqtts://aadd81fb96c3478d9cf64cf7a57edd8e.s1.eu.hivemq.cloud',
    {
        username: 'CE232',
        password: 'Thanh0512'
    }
);

mongoose.connection.on('connected', async() => {
    console.log('MongoDB connected');
});
mongoose.connection.on('error', async(err) => {
    console.log('MongoDB connection error',err);    
});

client.on('connect', async () => {
    await mongoose.connect('mongodb+srv://21520457:Thanh0512@cluster0.23obvsi.mongodb.net/?retryWrites=true&w=majority');
    console.log('MQTT Connected');
    client.subscribe(topic_sensor);
});

client.on('message', async (topic_sensor, message) => {
    console.log('MQTT received Topic:', topic_sensor.toString() ,', Message: ', message.toString());

    let data = message.toString();
    data = JSON.parse(data);
    data.created = moment().utc().add(5, 'hours');
    data._id = shortId.generate();
    await saveData(data);
}) 
saveData = async (data) => {
  data = new Events(data);
  data = await data.save();
  console.log('Saved data:', data);
}