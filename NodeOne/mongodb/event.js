const mongoose = require('mongoose');
const moment = require('moment');
const Schema = mongoose.Schema;

const EventsSchema = new Schema({
    Temperature: {
        type: Number,
        required: true
    },

    Humidity: {
        type: Number,
        required: true
    },
    
    created: {
        type: Date,
        ddefault: () => moment().utc().add(6, 'hours').toDate()
    }
}, {
        _id: false,
        id: false,
        versionKey: false,
        strict: false
    }
);

module.exports = mongoose.model('sensor', EventsSchema);