import React from 'react';
import { useState, useEffect } from 'react';

import 'bootstrap/dist/css/bootstrap.min.css';
import Container from 'react-bootstrap/Container';
import Row from 'react-bootstrap/Row';
import Col from 'react-bootstrap/Col';
import Card from 'react-bootstrap/Card';
import { ListGroup } from 'react-bootstrap';

import { useAdapterEndpoint } from 'odin-react';


// function Config(props) {
//     const {keyName, value} = props;

//     return (
//         <Card>
//             <ListGroup variant="flush">
//             <ListGroup.Item>{keyName}</ListGroup.Item>
//             <ListGroup.Item>{value}</ListGroup.Item>
//             </ListGroup>
//         </Card>
//     )
// }

function ConfigItem({ data, parentKey = 'config' }) {
    if (typeof data === 'object') {
        return (
            <Card>
                <Card.Header>{parentKey}</Card.Header>
                <ListGroup variant='flush'>
                    {Object.entries(data).map(([key, value]) => (
                        <ListGroup.Item key={key}>
                            <ConfigItem data={value} parentKey={key} />
                        </ListGroup.Item>
                    ))}
                </ListGroup>
            </Card>
        );
    } else {
        return (
            <div>
                {parentKey}: {String(data)}
            </div>
        );
    }
}



function PageTwo(props) {

    // const endpoint = props
    const endpoint = useAdapterEndpoint("camera_control",  import.meta.env.VITE_ENDPOINT_URL, 1000);

    const configData = endpoint.data?.cameras?.aravis?.config;

    // const ConfigList = endpoint.data?.cameras?.aravis?.config ? Object.entries(endpoint.data?.cameras.aravis.config).filter(([key, _]) => key !== 'trigger').map(
    //     ([key, value]) => {
    //         return (
    //         <Container>
    //             <p></p>
    //             <Row>
    //                 <Config keyName={key} value={value} />
    //             </Row>
    //         </Container>
    //         )
    //     }
    // ) : <></>

    return (

        <Container>
            {/* {ConfigList} */}
            <Row>
                <p></p>
                {configData ? <ConfigItem data={configData} /> : <p></p>}
            </Row>
        </Container>

    )
}

export default PageTwo;