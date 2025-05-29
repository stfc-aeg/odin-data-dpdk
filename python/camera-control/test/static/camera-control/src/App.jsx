import React from 'react'

import { useState } from 'react'
import reactLogo from './assets/react.svg'
// import viteLogo from '/vite.svg'
import './App.css'

import { OdinApp } from 'odin-react'
// import 'odin-react/dist/index.css'

import 'bootstrap/dist/css/bootstrap.min.css'
import Container from 'react-bootstrap/Container'
import InputGroup from 'react-bootstrap/InputGroup'

import { useAdapterEndpoint } from 'odin-react'
// import { StatusBox } from 'odin-react'
import { OdinEventLog } from 'odin-react'

function App() {
  // const [count, setCount] = useState(0)

  const endpoint = useAdapterEndpoint("camera_control/cameras/aravis",  import.meta.env.VITE_ENDPOINT_URL, 1000);

  const connected = endpoint.data?.aravis?.connection?.connected ? endpoint.data?.aravis?.connection?.connected : false;
  const state = endpoint.data?.aravis?.status?.state ? endpoint.data?.aravis?.status?.state : "disconnected";

  return (
    // <>
    //   <div>
    //     <a href="https://vite.dev" target="_blank">
    //       <img src={viteLogo} className="logo" alt="Vite logo" />
    //     </a>
    //     <a href="https://react.dev" target="_blank">
    //       <img src={reactLogo} className="logo react" alt="React logo" />
    //     </a>
    //   </div>
    //   <h1>Vite + React</h1>
    //   <div className="card">
    //     <button onClick={() => setCount((count) => count + 1)}>
    //       count is {count}
    //     </button>
    //     <p>
    //       Edit <code>src/App.jsx</code> and save to test HMR
    //     </p>
    //   </div>
    //   <p className="read-the-docs">
    //     Click on the Vite and React logos to learn more
    //   </p>
    // </>

    <OdinApp title="title" navLinks={["Page one", "Page two"]}>

      <Container>
        <p></p>
        <StatusBox label="connected" type={connected ? "success" : "danger"}>{connected}</StatusBox>
        <p></p>
        <InputGroup>
          <InputGroup.Text>state</InputGroup.Text>
          <InputGroup.Text>{state || "not found"}</InputGroup.Text>
        </InputGroup>
        <p></p>
        <OdinEventLog></OdinEventLog>
      </Container>

      <Container>Page two</Container>

    </OdinApp>
  )
}

export default App
