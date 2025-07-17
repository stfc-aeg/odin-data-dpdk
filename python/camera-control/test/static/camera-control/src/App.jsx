import React from 'react';

import { useState } from 'react';
import reactLogo from './assets/react.svg';
import 'bootstrap/dist/css/bootstrap.min.css';


import { OdinApp } from 'odin-react';

import Container from 'react-bootstrap/Container';
import InputGroup from 'react-bootstrap/InputGroup';

import { useAdapterEndpoint } from 'odin-react';
import { OdinEventLog } from 'odin-react';

import PageOne from './PageOne';
import PageTwo from './PageTwo';

function App() {

  const endpoint = useAdapterEndpoint("camera_control",  import.meta.env.VITE_ENDPOINT_URL, 1000);

  return (

    <OdinApp title="Camera Adapter" navLinks={["Preview", "Config"]}>

      <PageOne endpoint = {endpoint}/>
      <PageTwo endpoint = {endpoint}/>

    </OdinApp>
  )
}

export default App
