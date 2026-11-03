<?php
/** @var \CodeIgniter\Router\RouteCollection $routes */
$routes->setAutoRoute(false);
$routes->get('health', 'Feed::health');
$routes->get('feed', 'Feed::index');
$routes->get('posts/(:segment)', 'Feed::show/$1');
$routes->post('posts', 'Feed::create');
$routes->post('posts/(:segment)/like', 'Feed::like/$1');
$routes->set404Override('\\App\\Controllers\\Feed::notFound');
