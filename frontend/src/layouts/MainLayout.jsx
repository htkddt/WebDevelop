import { Outlet } from 'react-router-dom';
import Leftbar from '../components/Leftbar';
import Topbar from '../components/Topbar';
import ChatWidget from '../components/ChatWidget';
import './../styles/Frames.css';

export const MainLayout = () => {
  return (
    <div className="layout-container">
      <Topbar />
      <div className="layout-body">
        <Leftbar />
        <main className="layout-content">
          <Outlet />
        </main>
        <ChatWidget />
      </div>
    </div>
  );
};

export default MainLayout;