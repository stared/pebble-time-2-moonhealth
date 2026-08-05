module.exports = [
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Location'
      },
      {
        type: 'text',
        defaultValue: 'For sunrise/sunset times.'
      },
      {
        type: 'toggle',
        messageKey: 'UseGps',
        label: 'Use phone location',
        defaultValue: true
      },
      {
        type: 'text',
        defaultValue: 'Or set it manually (decimal degrees; south and west are negative):'
      },
      {
        type: 'input',
        messageKey: 'Lat',
        label: 'Latitude',
        attributes: {
          placeholder: 'e.g. 52.23',
          type: 'number',
          step: '0.01'
        }
      },
      {
        type: 'input',
        messageKey: 'Lon',
        label: 'Longitude',
        attributes: {
          placeholder: 'e.g. 21.01',
          type: 'number',
          step: '0.01'
        }
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];
