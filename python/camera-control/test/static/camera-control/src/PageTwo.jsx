import React from 'react';
import { useState, useEffect } from 'react';

import 'bootstrap/dist/css/bootstrap.min.css';
import Container from 'react-bootstrap/Container';
import Row from 'react-bootstrap/Row';
import Col from 'react-bootstrap/Col';
import Card from 'react-bootstrap/Card';
import { ListGroup } from 'react-bootstrap';
import Form from 'react-bootstrap/Form';

import { useAdapterEndpoint } from 'odin-react';
import { WithEndpoint } from 'odin-react';

// function ConfigItem({ data, parentKey = 'config' }) {
//     if (typeof data === 'object') {
//         return (
//             <Card>
//                 <Card.Header>{parentKey}</Card.Header>
//                 <ListGroup variant='flush'>
//                     {Object.entries(data).map(([key, value]) => (
//                         const currentPath = `${path}/${key}`;
//                         console.debug(`Config path: ${currentPath}`);
//                         return (
//                         <ListGroup.Item key={key}>
//                             <ConfigItem data={value} parentKey={key} />
//                         </ListGroup.Item>
//                         );
//                     ))}
//                 </ListGroup>
//             </Card>
//         );
//     } else {
//         return (
//             <div>
//                 {parentKey}: {String(data)}
//             </div>
//         );
//     }
// }

function ConfigItem({ data, parentKey = 'config', path = 'config' }) {
    if (typeof data === 'object' && data !== null) {
        return (
            <Card>
                <Card.Header>{parentKey}</Card.Header>
                <ListGroup variant='flush'>
                    {Object.entries(data).map(([key, value]) => {
                        const currentPath = `${path}/${key}`;
                        // console.debug(`Config path: ${currentPath}`);
                        return (
                            <ListGroup.Item key={currentPath}>
                                <ConfigItem data={value} parentKey={key} path={currentPath}/>
                            </ListGroup.Item>
                        );
                    })}
                </ListGroup>
            </Card>
        );
    } else {
        console.debug(`${String(parentKey)} Config path: ${path}`);
        return (
            <div>
                <p>{parentKey}: {String(data)}</p>
                <p>cameras/aravis/{path}</p>
            </div>
        );
    }
}


const EndpointInput = WithEndpoint(Form.Control)



function PageTwo(props) {

    // const endpoint = props
    // const endpoint = useAdapterEndpoint("camera_control",  import.meta.env.VITE_ENDPOINT_URL, 1000, {params: {wants_metadata: true}});
    const endpoint = useAdapterEndpoint("camera_control",  import.meta.env.VITE_ENDPOINT_URL, 1000);

    const configData = endpoint.data?.cameras?.aravis?.config;

    const metadata = endpoint.metadata ? endpoint.metadata : {};

    return (

        <Container>
            <Row>
                <p></p>
                {configData ? <ConfigItem data={configData} /> : <p></p>}
            </Row>
            <Row>
                <p></p>
                <Form.Label>Frame rate</Form.Label>
                <EndpointInput
                    endpoint={endpoint}
                    event_type="change"
                    type="number"
                    step={1}
                    fullpath={"cameras/aravis/config/frame_rate"}/>
                <p></p>
                <Form.Label>Exposure</Form.Label>
                <EndpointInput
                    endpoint={endpoint}
                    event_type="change"
                    type="number"
                    step={0.001}
                    fullpath={"cameras/aravis/config/exposure_time"}/>
            </Row>
            <Row>
                <p></p>
                <pre>{JSON.stringify(metadata, null, 2)}</pre>
            </Row>
        </Container>

    )
}

export default PageTwo;